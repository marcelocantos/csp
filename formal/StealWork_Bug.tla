---- MODULE StealWork_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates what happens when steal_work does NOT
 * check the running pointer before stealing.
 *
 * In the correct code (runtime.cpp:311), steal_work skips any candidate
 * that equals victim.running. Without this check, the thief can steal
 * the MT that the victim is actively executing, leading to the MT being
 * on the global queue while simultaneously being run by the victim.
 *
 * Expected result: TLC finds a RunningNotStealable violation.
 ******************************************************************************)

EXTENDS FiniteSets

CONSTANTS
    MTs,
    Victim,
    Thief

VARIABLES
    local,
    global,
    running,
    in_global,
    run_mu,
    global_mu,
    pc_victim,
    pc_thief,
    pc_waker,
    pc_taker,
    steal_cand,
    waker_mt

vars == <<local, global, running, in_global, run_mu, global_mu,
          pc_victim, pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

Procs == {Victim, Thief}

OnLocal(mt) == \E P \in Procs : mt \in local[P]
IsRunning(mt) == \E P \in Procs : running[P] = mt

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
 * VICTIM ACTIONS — identical to correct version.
 ******************************************************************************)

VLocalNext ==
    /\ pc_victim = "v_local_next"
    /\ run_mu[Victim] = "none"
    /\ IF local[Victim] /= {}
       THEN \E mt \in local[Victim] :
            /\ running' = [running EXCEPT ![Victim] = mt]
            /\ pc_victim' = "v_running"
       ELSE /\ running' = [running EXCEPT ![Victim] = "none"]
            /\ pc_victim' = "v_local_next"
    /\ UNCHANGED <<local, global, in_global, run_mu, global_mu,
                   pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

VDoSwitch ==
    /\ pc_victim = "v_running"
    /\ run_mu[Victim] = "none"
    /\ running[Victim] /= "none"
    /\ running' = [running EXCEPT ![Victim] = "none"]
    /\ pc_victim' = "v_local_next"
    /\ UNCHANGED <<local, global, in_global, run_mu, global_mu,
                   pc_thief, pc_waker, pc_taker, steal_cand, waker_mt>>

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
 * THIEF ACTIONS — identical except TStealCheck_Bug does NOT filter out
 * the running MT.
 ******************************************************************************)

TStealAcquireRunMu ==
    /\ pc_thief = "t_idle"
    /\ run_mu[Victim] = "none"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "thief"]
    /\ pc_thief' = "t_try_global"
    /\ UNCHANGED <<local, global, running, in_global, global_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

TStealTryGlobalOK ==
    /\ pc_thief = "t_try_global"
    /\ global_mu = "none"
    /\ global_mu' = "thief"
    /\ pc_thief' = "t_check_busy"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

TStealTryGlobalFail ==
    /\ pc_thief = "t_try_global"
    /\ global_mu /= "none"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global, global_mu,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(*******************************************************************************
 * BUG: TStealCheck does NOT exclude running[Victim] from candidates.
 *
 * In the correct code (runtime.cpp:311):
 *   if (candidate == victim.running) continue;
 *
 * Without this check, the thief considers ALL MTs on the victim's local
 * queue, including the one the victim is actively executing. Since both
 * the thief and victim hold run_mu, the thief sees the latest running
 * value — but in the buggy version, it doesn't check it.
 ******************************************************************************)
TStealCheck_Bug ==
    /\ pc_thief = "t_check_busy"
    \* BUG: candidates = ALL local MTs — no running filter
    /\ LET candidates == local[Victim]
       IN IF candidates /= {}
          THEN \E m \in candidates :
               /\ steal_cand' = m
               /\ pc_thief' = "t_delink_push"
          ELSE /\ steal_cand' = "none"
               /\ pc_thief' = "t_release_fail"
    /\ UNCHANGED <<local, global, running, in_global, run_mu, global_mu,
                   pc_victim, pc_waker, pc_taker, waker_mt>>

TStealReleaseFail ==
    /\ pc_thief = "t_release_fail"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ global_mu' = "none"
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

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

TStealReleaseOK ==
    /\ pc_thief = "t_release_ok"
    /\ run_mu' = [run_mu EXCEPT ![Victim] = "none"]
    /\ global_mu' = "none"
    /\ pc_thief' = "t_idle"
    /\ UNCHANGED <<local, global, running, in_global,
                   pc_victim, pc_waker, pc_taker, steal_cand, waker_mt>>

(*******************************************************************************
 * WAKER ACTIONS — identical to correct version.
 ******************************************************************************)

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

WAcquireLock ==
    /\ pc_waker = "w_want_lock"
    /\ global_mu = "none"
    /\ global_mu' = "sched"
    /\ pc_waker' = "w_push"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_thief, pc_taker, steal_cand, waker_mt>>

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
 * TAKE FROM GLOBAL ACTIONS — identical to correct version.
 ******************************************************************************)

TkAcquireGlobal ==
    /\ pc_taker = "tk_idle"
    /\ global /= {}
    /\ global_mu = "none"
    /\ global_mu' = "taker"
    /\ pc_taker' = "tk_pop"
    /\ UNCHANGED <<local, global, running, in_global, run_mu,
                   pc_victim, pc_thief, pc_waker, steal_cand, waker_mt>>

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
    \/ TStealCheck_Bug      \* Uses the buggy version (no running check)
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
 * PROPERTIES — same as correct version.
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

NoDoublePlacement ==
    \A mt \in MTs :
        /\ ~(mt \in local[Victim] /\ mt \in local[Thief])
        /\ ~(OnLocal(mt) /\ mt \in global)

NoDanglingState ==
    \A mt \in MTs :
        in_global[mt] => mt \in global

(* RunningNotStealable: thief never steals the running MT. *)
RunningNotStealable ==
    pc_thief \in {"t_delink_push", "t_release_ok"}
        => steal_cand /= running[Victim]

====
