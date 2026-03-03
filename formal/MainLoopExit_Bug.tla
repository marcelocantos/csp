---- MODULE MainLoopExit_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the lost-notification race when
 * destroy_imp skips the lock-unlock on park_mu before notify_all.
 *
 * Bug: the last imp to exit calls notify_all WITHOUT first acquiring
 * park_mu.  This allows:
 *   1. main_loop evaluates predicate (live_gs > 0, holds park_mu)
 *   2. Last imp decrements live_gs to 0
 *   3. Last imp calls notify_all — main_loop hasn't entered wait yet
 *   4. main_loop enters cv.wait — notification already consumed
 *   5. main_loop sleeps forever (deadlock)
 *
 * Expected result: TLC finds a NoLostNotification violation.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Imps

VARIABLES
    live_gs,
    park_mu,
    notified,
    imp_pc,
    main_pc

vars == <<live_gs, park_mu, notified, imp_pc, main_pc>>

(* Start with main_loop already in the CV wait region — the realistic
 * initial state when imps are alive (predicate was false, entered wait). *)
Init ==
    /\ live_gs = Cardinality(Imps)
    /\ park_mu = "none"
    /\ notified = FALSE
    /\ imp_pc = [i \in Imps |-> "alive"]
    /\ main_pc = "in_wait"

(*******************************************************************************
 * IMP ACTIONS — identical to correct version except: no lock-unlock
 * before notify.  Last imp goes directly from decrement to notify.
 ******************************************************************************)

ImpStartExit(i) ==
    /\ imp_pc[i] = "alive"
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "decrement"]
    /\ UNCHANGED <<live_gs, park_mu, notified, main_pc>>

(* BUG: last imp goes straight to notify, no lock-unlock. *)
ImpDecrement(i) ==
    /\ imp_pc[i] = "decrement"
    /\ live_gs' = live_gs - 1
    /\ imp_pc' = [imp_pc EXCEPT ![i] = IF live_gs = 1
                                        THEN "notify"     \* BUG: skip lock-unlock
                                        ELSE "dead"]
    /\ UNCHANGED <<park_mu, notified, main_pc>>

(* Notify without prior lock acquisition.
 * Only main_loop in "in_wait" receives the notification. *)
ImpNotify(i) ==
    /\ imp_pc[i] = "notify"
    /\ notified' = (main_pc = "in_wait")    \* Only delivered if main is waiting
    /\ imp_pc' = [imp_pc EXCEPT ![i] = "dead"]
    /\ UNCHANGED <<live_gs, park_mu, main_pc>>

(*******************************************************************************
 * MAIN LOOP ACTIONS — identical to the correct version.
 ******************************************************************************)

MainAcquire ==
    /\ main_pc = "acquire"
    /\ park_mu = "none"
    /\ park_mu' = "main"
    /\ main_pc' = "eval_pred"
    /\ UNCHANGED <<live_gs, notified, imp_pc>>

MainEvalPred ==
    /\ main_pc = "eval_pred"
    /\ park_mu = "main"
    /\ IF live_gs = 0
       THEN /\ park_mu' = "none"
            /\ main_pc' = "exited"
            /\ UNCHANGED <<live_gs, notified, imp_pc>>
       ELSE /\ park_mu' = "none"
            /\ main_pc' = "in_wait"
            /\ UNCHANGED <<live_gs, notified, imp_pc>>

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
    /\ imp_pc \in [Imps -> {"alive", "decrement", "notify", "dead"}]
    /\ main_pc \in {"acquire", "eval_pred", "in_wait", "woken", "exited"}

CorrectExit ==
    main_pc = "exited" => live_gs = 0

(* VIOLATED: main_loop can be stuck in in_wait with no notification. *)
NoLostNotification ==
    (\A i \in Imps : imp_pc[i] = "dead")
    => \/ main_pc \in {"exited", "acquire", "eval_pred"}
       \/ (main_pc = "in_wait" /\ notified)

EventualExit ==
    <>( main_pc = "exited" )

====
