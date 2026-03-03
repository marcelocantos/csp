---- MODULE MainLoopExit ----
(*******************************************************************************
 * Models the M:N main_loop exit protocol: how destroy_imp notifies
 * main_loop when the last imp exits (live_gs reaches 0).
 *
 * main_loop waits on park_cv with predicate live_gs==0.  destroy_imp
 * decrements live_gs with fetch_sub(acq_rel); when the result is 0,
 * it uses the lock-unlock+notify pattern on park_mu to ensure the
 * notification reaches main_loop.
 *
 * The race (same pattern as WorkerParking):
 *   main_loop evaluates predicate (live_gs > 0), about to call cv.wait.
 *   destroy_imp decrements live_gs to 0, calls notify_all.
 *   main_loop enters cv.wait — notification was already consumed.
 *   main_loop sleeps forever (deadlock: shutdown_runtime never proceeds).
 *
 * Fix: destroy_imp acquires+releases park_mu before notify_all,
 * ensuring main_loop has either entered wait (and will be woken) or
 * hasn't acquired park_mu yet (and will see live_gs==0 in predicate).
 *
 * Participants:
 *   Imps     — imps that exit via destroy_imp (decrement live_gs)
 *   MainLoop — main_loop waiting for live_gs==0
 *
 * Code references:
 *   ImpExit          — csp.cc:196 (live_gs.fetch_sub)
 *   ImpAcquireMu     — csp.cc:197 (lock_guard park_mu — only when last)
 *   ImpReleaseMu     — csp.cc:197 (} end of lock_guard)
 *   ImpNotify        — csp.cc:198 (park_cv.notify_all)
 *   MainAcquire      — runtime.cpp:177 (unique_lock park_mu)
 *   MainEvalPred     — runtime.cpp:178-179 (live_gs==0 predicate)
 *   MainEnterWait    — runtime.cpp:178 (park_cv.wait, releases park_mu)
 *   MainWoken        — runtime.cpp:178 (woken, re-acquires park_mu)
 *
 * Expected result: TLC finds no violations.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Imps        \* set of imp IDs, e.g. {i1, i2}

VARIABLES
    live_gs,        \* Int: count of live imps
    park_mu,        \* "none" | "main" | imp ID — who holds park_mu
    notified,       \* Boolean: notify_all has been called for the last imp
    imp_pc,         \* [Imps -> imp program counter]
    main_pc         \* main_loop program counter

vars == <<live_gs, park_mu, notified, imp_pc, main_pc>>

(*******************************************************************************
 * Imp program counters:
 *   "alive"       — imp is running
 *   "decrement"   — about to decrement live_gs
 *   "acquire_mu"  — last imp: about to acquire park_mu
 *   "release_mu"  — last imp: holding park_mu, about to release
 *   "notify"      — last imp: about to notify
 *   "dead"        — imp has exited
 *
 * Main program counters:
 *   "acquire"     — about to acquire park_mu
 *   "eval_pred"   — holding park_mu, evaluating predicate
 *   "in_wait"     — released park_mu, blocked in cv.wait
 *   "woken"       — woken, re-acquired park_mu
 *   "exited"      — main_loop returned (live_gs==0 confirmed)
 ******************************************************************************)

Init ==
    /\ live_gs = Cardinality(Imps)
    /\ park_mu = "none"
    /\ notified = FALSE
    /\ imp_pc = [i \in Imps |-> "alive"]
    /\ main_pc = "acquire"

(*******************************************************************************
 * IMP ACTIONS (destroy_imp path)
 ******************************************************************************)

(* Imp decides to exit.
 * TLA:MainLoopExit.ImpStartExit *)
ImpStartExit(i) ==
    /\ imp_pc[i] = "alive"
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "decrement"]
    /\ UNCHANGED <<live_gs, park_mu, notified, main_pc>>

(* Imp decrements live_gs.  If this is the last imp (was 1, now 0),
 * proceed to lock-unlock+notify.  Otherwise, done.
 *
 * Code: csp.cc:196
 *   if (rt.live_gs.fetch_sub(1, acq_rel) == 1) { ... }
 *
 * TLA:MainLoopExit.ImpDecrement *)
ImpDecrement(i) ==
    /\ imp_pc[i] = "decrement"
    /\ live_gs' = live_gs - 1
    /\ imp_pc' = [imp_pc EXCEPT ![i] = IF live_gs = 1
                                        THEN "acquire_mu"
                                        ELSE "dead"]
    /\ UNCHANGED <<park_mu, notified, main_pc>>

(* Last imp acquires park_mu (empty critical section).
 * Code: csp.cc:197 — { lock_guard<mutex> lk(rt.park_mu); }
 *
 * TLA:MainLoopExit.ImpAcquireMu *)
ImpAcquireMu(i) ==
    /\ imp_pc[i] = "acquire_mu"
    /\ park_mu = "none"
    /\ park_mu' = i
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "release_mu"]
    /\ UNCHANGED <<live_gs, notified, main_pc>>

(* Last imp releases park_mu.
 * Code: csp.cc:197 — } end of lock_guard
 *
 * TLA:MainLoopExit.ImpReleaseMu *)
ImpReleaseMu(i) ==
    /\ imp_pc[i] = "release_mu"
    /\ park_mu = i
    /\ park_mu' = "none"
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "notify"]
    /\ UNCHANGED <<live_gs, notified, main_pc>>

(* Last imp calls notify_all.
 * Code: csp.cc:198 — rt.park_cv.notify_all()
 *
 * TLA:MainLoopExit.ImpNotify *)
ImpNotify(i) ==
    /\ imp_pc[i] = "notify"
    /\ notified' = TRUE
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "dead"]
    /\ UNCHANGED <<live_gs, park_mu, main_pc>>

(*******************************************************************************
 * MAIN LOOP ACTIONS
 ******************************************************************************)

(* main_loop acquires park_mu.
 * Code: runtime.cpp:177 — unique_lock<mutex> lk(park_mu)
 *
 * TLA:MainLoopExit.MainAcquire *)
MainAcquire ==
    /\ main_pc = "acquire"
    /\ park_mu = "none"
    /\ park_mu' = "main"
    /\ main_pc' = "eval_pred"
    /\ UNCHANGED <<live_gs, notified, imp_pc>>

(* main_loop evaluates predicate: live_gs == 0.
 * Code: runtime.cpp:179 — live_gs.load(acquire) == 0
 *
 * TLA:MainLoopExit.MainEvalPred *)
MainEvalPred ==
    /\ main_pc = "eval_pred"
    /\ park_mu = "main"
    /\ IF live_gs = 0
       THEN \* Predicate true: exit
            /\ park_mu' = "none"
            /\ main_pc' = "exited"
            /\ UNCHANGED <<live_gs, notified, imp_pc>>
       ELSE \* Predicate false: enter wait
            /\ park_mu' = "none"
            /\ main_pc' = "in_wait"
            /\ UNCHANGED <<live_gs, notified, imp_pc>>

(* main_loop is woken by notify_all, re-acquires park_mu.
 * TLA:MainLoopExit.MainWoken *)
MainWoken ==
    /\ main_pc = "in_wait"
    /\ notified
    /\ park_mu = "none"
    /\ park_mu' = "main"
    /\ main_pc' = "eval_pred"
    /\ UNCHANGED <<live_gs, notified, imp_pc>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E i \in Imps :
        \/ ImpStartExit(i)
        \/ ImpDecrement(i)
        \/ ImpAcquireMu(i)
        \/ ImpReleaseMu(i)
        \/ ImpNotify(i)
    \/ MainAcquire
    \/ MainEvalPred
    \/ MainWoken

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ live_gs \in 0..Cardinality(Imps)
    /\ park_mu \in ({"none", "main"} \cup Imps)
    /\ notified \in BOOLEAN
    /\ imp_pc \in [Imps -> {"alive", "decrement", "acquire_mu",
                             "release_mu", "notify", "dead"}]
    /\ main_pc \in {"acquire", "eval_pred", "in_wait", "woken", "exited"}

(* Safety: main_loop only exits when all imps are dead. *)
CorrectExit ==
    main_pc = "exited" => live_gs = 0

(* Safety: after the last imp notifies, main_loop either has already
 * exited, hasn't entered the CV region yet (will see live_gs==0 in
 * predicate), is evaluating the predicate (will see live_gs==0), or
 * is in_wait and has a pending notification. *)
NoLostNotification ==
    (\A i \in Imps : imp_pc[i] = "dead")
    => \/ main_pc \in {"exited", "acquire", "eval_pred"}
       \/ (main_pc = "in_wait" /\ notified)

(* Liveness: main_loop eventually exits. *)
EventualExit ==
    <>( main_pc = "exited" )

====
