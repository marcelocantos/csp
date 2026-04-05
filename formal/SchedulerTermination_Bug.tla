---- MODULE SchedulerTermination_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the scheduler premature-exit race.
 *
 * Models the interaction between the reactor's fire_signal() (two-step:
 * push_to_global then decrement pending_signals) and the single-P
 * scheduler's exit decision in main_loop_scheduler().
 *
 * Bug: the scheduler exits when pending_signals reaches zero, without
 * checking has_global_work.  This misses the window where fire_signal
 * has pushed an imp to the global queue (setting has_global_work) but
 * hasn't yet decremented pending_signals.  If the scheduler drains and
 * runs that imp between the push and decrement, has_global_work is
 * cleared, then the next signal's push+decrement can leave an imp on
 * the global queue with pending_signals = 0 → premature exit.
 *
 * Participants:
 *   Reactor   — fires signals in fire_signal() (two steps each)
 *   Scheduler — single-P scheduler loop (drain, run, exit check)
 *
 * Code references:
 *   ReactorPush  — reactor.cc:148-162 (fire_signal under signal_mu)
 *                   erase writer → ~writer → schedule → push_to_global
 *   ReactorDecr  — reactor.cc:163-164 (pending_signals -= 1)
 *   SchedDrain   — csp.cc:439-448 (drain global → local, clear flag)
 *   SchedRunOne  — csp.cc:450-465 (run one local imp)
 *   SchedCheck   — csp.cc:17-36 (scheduler loop exit checks)
 *
 * Expected result: TLC finds a NoPrematureExit violation.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Signals     \* set of signal IDs, e.g. {s1, s2}

VARIABLES
    pending_signals,    \* Int: count of unfired signals (created - decremented)
    has_global_work,    \* Boolean: set by push_to_global, cleared by drain
    global_count,       \* Int: imps currently on global run queue
    local_count,        \* Int: imps currently on local run queue
    live_gs,            \* Int: imps that haven't exited
    sig_state,          \* [Signals -> {"pending","pushed","fired"}]
    sched_pc            \* Scheduler program counter

vars == <<pending_signals, has_global_work, global_count, local_count,
          live_gs, sig_state, sched_pc>>

(*******************************************************************************
 * Signal states:
 *   "pending"  — timer registered, imp blocked waiting for it
 *   "pushed"   — fire_signal erased writer (imp pushed to global queue,
 *                has_global_work set), but pending_signals NOT decremented
 *   "fired"    — pending_signals decremented, fire complete
 *
 * Each signal corresponds to one imp blocked on a timer.  When the signal
 * fires, the imp is scheduled to the global queue; when the scheduler
 * drains and runs it, the imp exits (live_gs decrements).
 ******************************************************************************)

Init ==
    /\ pending_signals = Cardinality(Signals)
    /\ has_global_work = FALSE
    /\ global_count = 0
    /\ local_count = 0
    /\ live_gs = Cardinality(Signals)
    /\ sig_state = [s \in Signals |-> "pending"]
    /\ sched_pc = "drain"

(*******************************************************************************
 * REACTOR ACTIONS
 *
 * fire_signal is two steps per signal, separated by the signal_mu
 * unlock.  Between the steps, the scheduler can drain the global
 * queue and run the imp.
 ******************************************************************************)

(* Step 1: erase writer → destructor → schedule → push_to_global.
 * All happens under signal_mu (the push acquires+releases global_mu
 * within the signal_mu scope).
 *
 * Code: reactor.cc:148-162
 *   { lock signal_mu; timer_writers_.erase(ident); }
 *   erase destroys writer → ~writer → schedule() → {lock global_mu;
 *   push_to_global; has_global_work_=true; unlock global_mu}
 *
 * TLA:SchedulerTermination_Bug.ReactorPush *)
ReactorPush(s) ==
    /\ sig_state[s] = "pending"
    /\ global_count' = global_count + 1
    /\ has_global_work' = TRUE
    /\ sig_state' = [sig_state EXCEPT ![s] = "pushed"]
    /\ UNCHANGED <<pending_signals, local_count, live_gs, sched_pc>>

(* Step 2: decrement pending_signals (outside signal_mu).
 *
 * Code: reactor.cc:163-164
 *   if (erased) pending_signals_.fetch_sub(1, release);
 *
 * TLA:SchedulerTermination_Bug.ReactorDecr *)
ReactorDecr(s) ==
    /\ sig_state[s] = "pushed"
    /\ pending_signals' = pending_signals - 1
    /\ sig_state' = [sig_state EXCEPT ![s] = "fired"]
    /\ UNCHANGED <<has_global_work, global_count, local_count, live_gs, sched_pc>>

(*******************************************************************************
 * SCHEDULER ACTIONS
 *
 * Single-P scheduler loop (main_loop_scheduler):
 *   while (true) {
 *       if (run()) continue;
 *       if (live_gs == 0) break;
 *       if (!mn_mode_ && !has_pending_signals()) break;   // BUG
 *       park on CV;
 *   }
 *
 * run() = drain global queue + execute one local imp + return has_more.
 ******************************************************************************)

(* Drain global queue to local queue, clear has_global_work.
 * Serialized with ReactorPush by global_mu.
 *
 * Code: csp.cc:439-448
 *   lock(global_mu);
 *   while (!empty()) { pop → schedule_local; }
 *   has_global_work_ = false;
 *   unlock(global_mu);
 *
 * If nothing to drain and nothing local, go to exit check.
 * Otherwise, proceed to run an imp.
 *
 * TLA:SchedulerTermination_Bug.SchedDrain *)
SchedDrain ==
    /\ sched_pc = "drain"
    /\ local_count' = local_count + global_count
    /\ global_count' = 0
    /\ has_global_work' = FALSE
    /\ sched_pc' = IF local_count + global_count > 0
                    THEN "run_one"
                    ELSE "check_exit"
    /\ UNCHANGED <<pending_signals, live_gs, sig_state>>

(* Run one local imp.  In this model, each imp runs to completion
 * when executed (exits, decrementing live_gs).
 *
 * Code: csp.cc:450-465
 *   target = busy; target->run();
 *   // imp runs until it yields/blocks/exits
 *
 * After running, if more local work → stay in run_one.
 * Otherwise → back to drain (which will go to check_exit if
 * nothing new arrived on global).
 *
 * TLA:SchedulerTermination_Bug.SchedRunOne *)
SchedRunOne ==
    /\ sched_pc = "run_one"
    /\ local_count > 0
    /\ local_count' = local_count - 1
    /\ live_gs' = live_gs - 1
    /\ sched_pc' = IF local_count - 1 > 0 THEN "run_one" ELSE "drain"
    /\ UNCHANGED <<pending_signals, has_global_work, global_count, sig_state>>

(* Exit check.  BUG: only checks pending_signals, not has_global_work.
 *
 * Code (buggy): csp.cc:19,27-28
 *   if (live_gs == 0) break;
 *   if (!mn_mode_ && !has_pending_signals()) break;  // MISSING has_global_work
 *   park;
 *
 * TLA:SchedulerTermination_Bug.SchedCheckExit *)
SchedCheckExit ==
    /\ sched_pc = "check_exit"
    /\ IF live_gs = 0
       THEN sched_pc' = "exited"
       ELSE IF pending_signals = 0
            THEN sched_pc' = "exited"          \* BUG: no has_global_work check
            ELSE sched_pc' = "parked"
    /\ UNCHANGED <<pending_signals, has_global_work, global_count, local_count,
                   live_gs, sig_state>>

(* Wake from park_cv.  The CV predicate is:
 *   live_gs == 0 || has_global_work
 *
 * Code: csp.cc:31-35
 *   park_cv.wait(lk, [&rt] {
 *       return live_gs == 0 || has_global_work;
 *   });
 *
 * TLA:SchedulerTermination_Bug.SchedWake *)
SchedWake ==
    /\ sched_pc = "parked"
    /\ \/ live_gs = 0
       \/ has_global_work
    /\ sched_pc' = "drain"
    /\ UNCHANGED <<pending_signals, has_global_work, global_count, local_count,
                   live_gs, sig_state>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E s \in Signals :
        \/ ReactorPush(s)
        \/ ReactorDecr(s)
    \/ SchedDrain
    \/ SchedRunOne
    \/ SchedCheckExit
    \/ SchedWake

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ pending_signals \in 0..Cardinality(Signals)
    /\ has_global_work \in BOOLEAN
    /\ global_count \in 0..Cardinality(Signals)
    /\ local_count \in 0..Cardinality(Signals)
    /\ live_gs \in 0..Cardinality(Signals)
    /\ sig_state \in [Signals -> {"pending", "pushed", "fired"}]
    /\ sched_pc \in {"drain", "run_one", "check_exit", "parked", "exited"}

(* Safety: the scheduler must not exit while imps are still alive.
 *
 * In this model every imp has a pending timer that will fire, so every
 * imp WILL eventually be runnable.  Therefore the only correct exit is
 * when all imps have been run to completion (live_gs = 0). *)
NoPrematureExit ==
    sched_pc = "exited" => live_gs = 0

====
