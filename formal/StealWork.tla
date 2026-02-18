---- MODULE StealWork ----
(*******************************************************************************
 * Models the work-stealing dual-lock protocol from csp/src/runtime.cpp.
 *
 * The protocol ensures that when a thief steals a microthread (MT) from a
 * victim processor's local queue, the MT is never lost, duplicated, or
 * stolen while actively executing.
 *
 * Participants:
 *   Victim — a worker thread running MTs from its local queue (P0)
 *   Thief  — another worker thread that steals work from the victim (P1)
 *   Waker  — a thread that calls schedule() to push an MT to global queue
 *
 * Key modeling insight: in the real code, the running MT stays on the
 * local DLL. The `running` pointer is the ONLY thing that prevents
 * steal_work from taking it. We model this by keeping the running MT
 * in local[Victim] while also recording it in running[Victim].
 *
 * Code references:
 *   DoSwitch          — src/csp.cc:244-265    (set running under run_mu)
 *   LocalNext         — src/runtime.cpp:224-244 (pick next, set running)
 *   Deschedule        — src/csp.cc:149-160    (remove from DLL)
 *   StealAcquireLocks — src/runtime.cpp:298-304 (victim.run_mu + try global_mu)
 *   StealCheck        — src/runtime.cpp:306-313 (check busy, skip running)
 *   StealDelink       — src/runtime.cpp:318-322 (delink + push_to_global)
 *   TakeFromGlobal    — src/runtime.cpp:246-263 (pop global, schedule_local)
 *   Schedule          — src/csp.cc:120-143     (push to global under global_mu)
 ******************************************************************************)

EXTENDS FiniteSets

CONSTANTS
    MTs,        \* Set of microthread IDs (e.g., {m1, m2})
    Victim,     \* Processor ID for the victim
    Thief       \* Processor ID for the thief

VARIABLES
    local,      \* local[P] = set of MTs on P's local DLL (includes running MT)
    global,     \* Set of MTs on the global run queue
    running,    \* running[P] = MT currently executing on P, or "none"
    in_global,  \* in_global[mt] = boolean per MT
    run_mu,     \* run_mu[P] = "none" | "thief" — who holds P's mutex
    global_mu,  \* "none" | "thief" | "sched" | "taker"
    pc_victim,  \* Victim's program counter
    pc_thief,   \* Thief's program counter
    pc_waker,   \* Waker's program counter
    pc_taker,   \* TakeFromGlobal program counter
    steal_cand, \* Candidate MT chosen by thief for stealing
    waker_mt    \* MT the waker has decided to schedule

vars == <<local, global, running, in_global, run_mu, global_mu,
          pc_victim, pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

Procs == {Victim, Thief}

(*******************************************************************************
 * Helpers
 ******************************************************************************)

OnLocal(mt) == \E P \in Procs : mt \in local[P]
IsRunning(mt) == \E P \in Procs : running[P] = mt

(*******************************************************************************
 * Initial state: both MTs on the victim's local queue.
 ******************************************************************************)
Init ==
    /\ local = [P \in Procs |-> IF P = Victim THEN MTs ELSE {}]
    /\ global = {}
    /\ running = [P \in Procs |-> "none"]
    /\ in_global = [mt \in MTs |-> FALSE]
    /\ run_mu = [P \in Procs |-> "none"]
    /\ global_mu = "none"
    /\ pc_victim = "v_local_next"
    /\ pc_thief = "t_idle"
    /\ pc_waker = "w_idle"
    /\ pc_taker = "tk_idle"
    /\ steal_cand = "none"
    /\ waker_mt = "none"

(*******************************************************************************
 * VICTIM ACTIONS
 *
 * In the real code, the running MT stays on the DLL. local_next picks
 * a candidate, sets running = candidate (under run_mu), and returns it.
 * The MT stays on the DLL while running. When it yields/blocks,
 * do_switch updates running under run_mu.
 ******************************************************************************)

(* LocalNext: pick an MT from local queue, set as running. The MT
 * remains on the DLL — running just marks it as "claimed."
 *
 * Code: runtime.cpp:224-244
 *   std::lock_guard<std::mutex> lk(p.run_mu);
 *   ... candidate = busy; ...
 *   p.running = candidate;
 *   return candidate; *)
VLocalNext ==
    /\ pc_victim = "v_local_next"
    /\ run_mu[Victim] = "none"
    /\ IF local[Victim] /= {}
       THEN \E mt \in local[Victim] :
            /\ running' = [running EXCEPT ![Victim] = mt]
            \* MT stays on local — running is the only guard
            /\ pc_victim' = "v_running"
       ELSE /\ running' = [running EXCEPT ![Victim] = "none"]
            /\ pc_victim' = "v_local_next"
    /\ UNCHANGED <<local, global, in_global, run_mu, global_mu,
                   pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

(* DoSwitch: running MT yields, victim picks next MT to run. Under
 * run_mu, running is updated to g_self (the MT calling do_switch) and
 * then the busy pointer advances. We model this as: clear running
 * (the old MT is still on DLL, now stealable) and return to local_next.
 *
 * Code: src/csp.cc:244-265 (do_switch)
 *   std::lock_guard<std::mutex> lk(current_p().run_mu);
 *   current_p().running = g_self;
 *   ... busy = busy->next_;
 *   target = busy;
 *   ... target->run(); *)
VDoSwitch ==
    /\ pc_victim = "v_running"
    /\ run_mu[Victim] = "none"
    /\ running[Victim] /= "none"
    /\ running' = [running EXCEPT ![Victim] = "none"]
    /\ pc_victim' = "v_local_next"
    /\ UNCHANGED <<local, global, in_global, run_mu, global_mu,
                   pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

(* VDeschedule: running MT blocks (channel wait / sleep). It is removed
 * from the DLL and running is cleared. The MT becomes unplaced.
 *
 * Code: src/csp.cc:176-216 (run with Status::detach)
 *   Under run_mu: deschedule g_self (remove from DLL, next_=nullptr) *)
VDeschedule ==
    /\ pc_victim = "v_running"
    /\ run_mu[Victim] = "none"
    /\ running[Victim] /= "none"
    /\ LET mt == running[Victim]
       IN  /\ local' = [local EXCEPT ![Victim] = @ \ {mt}]
           /\ running' = [running EXCEPT ![Victim] = "none"]
    /\ pc_victim' = "v_local_next"
    /\ UNCHANGED <<global, in_global, run_mu, global_mu,
                   pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

(*******************************************************************************
 * THIEF ACTIONS
 *
 * steal_work protocol: acquire victim.run_mu, try global_mu, check
 * busy queue skipping running MT, delink+push under both locks.
 ******************************************************************************)

(* Acquire victim.run_mu.
 * Code: runtime.cpp:298 *)
TStealAcquireRunMu ==
    /\ pc_thief = "t_idle"
    /\ run_mu[Victim] = "none"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "thief"]
    /\ pc_thief' = "t_try_global"
    /\ UNCHANGED <<local, global, running, in_global, global_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(* try_to_lock on global_mu succeeds.
 * Code: runtime.cpp:303 *)
TStealTryGlobalOK ==
    /\ pc_thief = "t_try_global"
    /\ global_mu = "none"
    /\ global_mu' = "thief"
    /\ pc_thief' = "t_check_busy"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(* try_to_lock fails — release run_mu, back off.
 * Code: runtime.cpp:304 *)
TStealTryGlobalFail ==
    /\ pc_thief = "t_try_global"
    /\ global_mu /= "none"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global, global_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(* Check busy queue (both locks held). Skip the running MT.
 *
 * Code: runtime.cpp:306-313
 *   if (!victim.busy) continue;
 *   auto* candidate = victim.busy->prev_;
 *   if (candidate == victim.running) continue; *)
TStealCheck ==
    /\ pc_thief = "t_check_busy"
    /\ LET candidates == local[Victim] \ (IF running[Victim] /= "none"
                                           THEN {running[Victim]}
                                           ELSE {})
       IN IF candidates /= {}
          THEN \E m \in candidates :
               /\ steal_cand' = m
               /\ pc_thief' = "t_delink_push"
          ELSE /\ steal_cand' = "none"
               /\ pc_thief' = "t_release_fail"
    /\ UNCHANGED <<local, global, running, in_global, run_mu, global_mu,
                   pc_victim, pc_waker, pc_taker, waker_mt>>

(* Release both locks — no candidate found. *)
TStealReleaseFail ==
    /\ pc_thief = "t_release_fail"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ global_mu' = "none"
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(* Delink from victim DLL + push to global. Both locks held — atomic.
 * Code: runtime.cpp:318-322 *)
TStealDelinkPush ==
    /\ pc_thief = "t_delink_push"
    /\ steal_cand /= "none"
    /\ steal_cand \in local[Victim]
    /\ local' = [local EXCEPT ![Victim] = @ \ {steal_cand}]
    /\ global' = global \union {steal_cand}
    /\ in_global' = [in_global EXCEPT ![steal_cand] = TRUE]
    /\ pc_thief' = "t_release_ok"
    /\ UNCHANGED <<running, run_mu, global_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(* Release both locks after successful steal. *)
TStealReleaseOK ==
    /\ pc_thief = "t_release_ok"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ global_mu' = "none"
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(*******************************************************************************
 * WAKER ACTIONS
 *
 * schedule() on an unplaced MT: acquire global_mu, push to global.
 ******************************************************************************)

(* Choose an unplaced MT to schedule. An MT is unplaced when it has been
 * descheduled (removed from DLL, not running, not on global). *)
WStartSchedule ==
    /\ pc_waker = "w_idle"
    /\ \E mt \in MTs :
        /\ ~OnLocal(mt)
        /\ mt \notin global
        /\ ~IsRunning(mt)
        /\ ~in_global[mt]
        /\ waker_mt' = mt
        /\ pc_waker' = "w_want_lock"
    /\ UNCHANGED <<local, global, running, in_global, run_mu, global_mu,
                   pc_victim, pc_thief, pc_taker, steal_cand>>

(* Acquire global_mu.
 * Code: src/csp.cc:128 *)
WAcquireLock ==
    /\ pc_waker = "w_want_lock"
    /\ global_mu = "none"
    /\ global_mu' = "sched"
    /\ pc_waker' = "w_push"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_thief, pc_taker, steal_cand, waker_mt>>

(* Push MT to global queue under global_mu. Release lock.
 * Code: src/csp.cc:140 *)
WPush ==
    /\ pc_waker = "w_push"
    /\ waker_mt /= "none"
    /\ global' = global \union {waker_mt}
    /\ in_global' = [in_global EXCEPT ![waker_mt] = TRUE]
    /\ global_mu' = "none"
    /\ pc_waker' = "w_idle"
    /\ waker_mt' = "none"
    /\ UNCHANGED <<local, running, run_mu,
                   pc_victim, pc_thief, pc_taker, steal_cand>>

(*******************************************************************************
 * TAKE FROM GLOBAL ACTIONS
 ******************************************************************************)

(* Acquire global_mu.
 * Code: runtime.cpp:247 *)
TkAcquireGlobal ==
    /\ pc_taker = "tk_idle"
    /\ global /= {}
    /\ global_mu = "none"
    /\ global_mu' = "taker"
    /\ pc_taker' = "tk_pop"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_thief, pc_waker, steal_cand, waker_mt>>

(* Pop from global, schedule_local on thief's P. global_mu held
 * throughout (opposite lock order from steal_work — try_to_lock
 * prevents deadlock).
 * Code: runtime.cpp:257-261 *)
TkPopAndSchedule ==
    /\ pc_taker = "tk_pop"
    /\ run_mu[Thief] = "none"
    /\ global /= {}
    /\ \E mt \in global :
        /\ global' = global \ {mt}
        /\ in_global' = [in_global EXCEPT ![mt] = FALSE]
        /\ local' = [local EXCEPT ![Thief] = @ \union {mt}]
    /\ global_mu' = "none"
    /\ pc_taker' = "tk_idle"
    /\ UNCHANGED <<running, run_mu, pc_victim, pc_thief, pc_waker,
                   steal_cand, waker_mt>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ VLocalNext
    \/ VDoSwitch
    \/ VDeschedule
    \/ TStealAcquireRunMu
    \/ TStealTryGlobalOK
    \/ TStealTryGlobalFail
    \/ TStealCheck
    \/ TStealReleaseFail
    \/ TStealDelinkPush
    \/ TStealReleaseOK
    \/ WStartSchedule
    \/ WAcquireLock
    \/ WPush
    \/ TkAcquireGlobal
    \/ TkPopAndSchedule

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ \A P \in Procs : local[P] \subseteq MTs
    /\ global \subseteq MTs
    /\ \A P \in Procs : running[P] \in MTs \union {"none"}
    /\ \A mt \in MTs : in_global[mt] \in BOOLEAN
    /\ \A P \in Procs : run_mu[P] \in {"none", "thief"}
    /\ global_mu \in {"none", "thief", "sched", "taker"}
    /\ pc_victim \in {"v_local_next", "v_running"}
    /\ pc_thief \in {"t_idle", "t_try_global", "t_check_busy",
                      "t_delink_push", "t_release_ok", "t_release_fail"}
    /\ pc_waker \in {"w_idle", "w_want_lock", "w_push"}
    /\ pc_taker \in {"tk_idle", "tk_pop"}
    /\ steal_cand \in MTs \union {"none"}
    /\ waker_mt \in MTs \union {"none"}

(* NoDoublePlacement: every MT is on at most one queue (local or global).
 * The running MT is on the local queue AND marked as running — that's
 * by design (running just marks which local MT is active). But an MT
 * should never be on both a local queue and the global queue, or on
 * two local queues. *)
NoDoublePlacement ==
    \A mt \in MTs :
        \* Not on two different local queues
        /\ ~(mt \in local[Victim] /\ mt \in local[Thief])
        \* Not on a local queue and the global queue simultaneously
        /\ ~(OnLocal(mt) /\ mt \in global)

(* NoDanglingState: in_global => actually in global queue. *)
NoDanglingState ==
    \A mt \in MTs :
        in_global[mt] => mt \in global

(* RunningNotStealable: the thief's chosen steal candidate is never the
 * victim's currently running MT. *)
RunningNotStealable ==
    pc_thief \in {"t_delink_push", "t_release_ok"}
        => steal_cand /= running[Victim]

====
