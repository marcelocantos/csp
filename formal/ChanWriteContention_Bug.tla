---- MODULE ChanWriteContention_Bug ----
(*******************************************************************************
 * Bug variant of ChanWriteContention.
 *
 * Drops the `suspending_` short-circuit in schedule(). In the real code,
 * schedule() checks if the target is in the unlock_all → drain_suspended
 * window; if so, it sets wake_pending_ and returns (deferred wake). This
 * bug variant forgets the suspending_ check and tries to push to global
 * unconditionally, which is unsafe — the imp may not yet have completed
 * its do_switch, so concurrent execution can happen.
 *
 * For TLC, the visible failure is more subtle than double-execution: we
 * model the buggy schedule as PUSHING to global even when suspending_ is
 * true. The drain_suspended path then sees in_global=TRUE and the
 * wake_pending=FALSE branch, so it does NOT push. The result: when the
 * suspend window races with a wake, the imp ends up in_global, gets
 * picked up and run, but the drain ALSO clears suspending_ and the
 * writer eventually returns from do_switch — into a state where it's
 * already past the chan op. This is the double-execution bug, masked in
 * our model as a state where pc_writer is "running" twice (modeled by
 * tracking running_count).
 *
 * Expected TLC result: NoLostWriter still holds (writer does get
 * scheduled), but the new DoubleExecution invariant fails — the writer
 * is concurrently in two states.
 *
 * This proves the spec is actually checking schedule() short-circuits;
 * remove the bug ↔ remove the violation.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Writers

VARIABLES
    chan_mu,
    waiters,
    pc_writer,
    alt_state,
    in_global,
    in_local,
    suspending,
    wake_pending,
    running,
    signal,
    pc_reader,
    matched_w,
    global_mu,
    \* New variable for the bug variant: tracks if a writer was scheduled
    \* via the buggy path during its suspending window.
    sched_during_suspend

vars == <<chan_mu, waiters, pc_writer, alt_state, in_global, in_local,
          suspending, wake_pending, running, signal,
          pc_reader, matched_w, global_mu, sched_during_suspend>>

Init ==
    /\ chan_mu      = "none"
    /\ waiters      = {}
    /\ pc_writer    = [w \in Writers |-> "start"]
    /\ alt_state    = [w \in Writers |-> "idle"]
    /\ in_global    = [w \in Writers |-> FALSE]
    /\ in_local     = [w \in Writers |-> FALSE]
    /\ suspending   = [w \in Writers |-> FALSE]
    /\ wake_pending = [w \in Writers |-> FALSE]
    /\ running      = [w \in Writers |-> TRUE]
    /\ signal       = [w \in Writers |-> "none"]
    /\ pc_reader    = "start"
    /\ matched_w    = "none"
    /\ global_mu    = "none"
    /\ sched_during_suspend = [w \in Writers |-> FALSE]

(* All actions identical to ChanWriteContention except ReaderAltEndSchedule
 * is replaced with a buggy version that omits the suspending check. *)

WriterLockChan(w) ==
    /\ pc_writer[w] = "start"
    /\ chan_mu = "none"
    /\ chan_mu' = w
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "lock_chan"]
    /\ UNCHANGED <<waiters, alt_state, in_global, in_local, suspending,
                   wake_pending, running, signal, pc_reader, matched_w,
                   global_mu, sched_during_suspend>>

WriterRegister(w) ==
    /\ pc_writer[w] = "lock_chan"
    /\ chan_mu = w
    /\ waiters' = waiters \cup {w}
    /\ alt_state' = [alt_state EXCEPT ![w] = "waiting"]
    /\ suspending' = [suspending EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "registered"]
    /\ UNCHANGED <<chan_mu, in_global, in_local, wake_pending,
                   running, signal, pc_reader, matched_w, global_mu,
                   sched_during_suspend>>

WriterSwitchOut(w) ==
    /\ pc_writer[w] = "registered"
    /\ chan_mu = w
    /\ chan_mu' = "none"
    /\ running' = [running EXCEPT ![w] = FALSE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "switched_out"]
    /\ UNCHANGED <<waiters, alt_state, in_global, in_local, suspending,
                   wake_pending, signal, pc_reader, matched_w, global_mu,
                   sched_during_suspend>>

WriterDrain(w) ==
    /\ pc_writer[w] = "switched_out"
    /\ global_mu = "none"
    /\ global_mu' = "drain"
    /\ suspending' = [suspending EXCEPT ![w] = FALSE]
    /\ IF wake_pending[w] /\ ~in_global[w]
       THEN /\ wake_pending' = [wake_pending EXCEPT ![w] = FALSE]
            /\ in_global' = [in_global EXCEPT ![w] = TRUE]
       ELSE /\ wake_pending' = [wake_pending EXCEPT ![w] = FALSE]
            /\ UNCHANGED in_global
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "drained"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_local, running, signal,
                   pc_reader, matched_w, sched_during_suspend>>

WriterDrainRelease(w) ==
    /\ pc_writer[w] = "drained"
    /\ global_mu = "drain"
    /\ global_mu' = "none"
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "asleep"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   pc_reader, matched_w, sched_during_suspend>>

WriterPickedUp(w) ==
    /\ pc_writer[w] \in {"asleep", "drained"}
    /\ alt_state[w] = "claimed"
    /\ in_global[w]
    /\ in_global' = [in_global EXCEPT ![w] = FALSE]
    /\ in_local' = [in_local EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "woken"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, suspending, wake_pending,
                   running, signal, pc_reader, matched_w, global_mu,
                   sched_during_suspend>>

WriterRun(w) ==
    /\ pc_writer[w] = "woken"
    /\ in_local[w]
    /\ in_local' = [in_local EXCEPT ![w] = FALSE]
    /\ running' = [running EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "done"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_global, suspending,
                   wake_pending, signal, pc_reader, matched_w, global_mu,
                   sched_during_suspend>>

ReaderLockChan ==
    /\ pc_reader = "start"
    /\ chan_mu = "none"
    /\ chan_mu' = "reader"
    /\ pc_reader' = "scanning"
    /\ UNCHANGED <<waiters, pc_writer, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   matched_w, global_mu, sched_during_suspend>>

ReaderClaim ==
    /\ pc_reader = "scanning"
    /\ chan_mu = "reader"
    /\ \E w \in waiters :
        /\ alt_state[w] = "waiting"
        /\ alt_state' = [alt_state EXCEPT ![w] = "claimed"]
        /\ signal' = [signal EXCEPT ![w] = "matched"]
        /\ matched_w' = w
        /\ waiters' = waiters \ {w}
    /\ pc_reader' = "matched"
    /\ UNCHANGED <<chan_mu, pc_writer, in_global, in_local, suspending,
                   wake_pending, running, global_mu, sched_during_suspend>>

ReaderUnlockChan ==
    /\ pc_reader = "matched"
    /\ chan_mu = "reader"
    /\ chan_mu' = "none"
    /\ pc_reader' = "altend"
    /\ UNCHANGED <<waiters, pc_writer, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   matched_w, global_mu, sched_during_suspend>>

(* BUG: Reader's altend schedule omits the suspending_ check.
 * If the matched writer is in suspending=TRUE, the buggy schedule
 * pushes to global anyway. This sets in_global=TRUE before the writer
 * has finished its do_switch — modelling double-execution.
 *
 * The Sched_during_suspend flag tracks when this happens. *)
ReaderAltEndScheduleBuggy ==
    /\ pc_reader = "altend"
    /\ global_mu = "none"
    /\ matched_w /= "none"
    /\ LET w == matched_w IN
        /\ global_mu' = "sched"
        /\ IF in_global[w]
           THEN UNCHANGED <<in_global, wake_pending, sched_during_suspend>>
           ELSE IF in_local[w]
                THEN UNCHANGED <<in_global, wake_pending, sched_during_suspend>>
                ELSE \* BUG: no suspending check — push regardless
                     /\ in_global' = [in_global EXCEPT ![w] = TRUE]
                     /\ sched_during_suspend' = [sched_during_suspend EXCEPT
                          ![w] = suspending[w] \/ sched_during_suspend[w]]
                     /\ UNCHANGED wake_pending
    /\ pc_reader' = "altend_release"
    /\ UNCHANGED <<chan_mu, waiters, pc_writer, alt_state, in_local,
                   suspending, running, signal, matched_w>>

ReaderAltEndRelease ==
    /\ pc_reader = "altend_release"
    /\ global_mu = "sched"
    /\ global_mu' = "none"
    /\ pc_reader' = "done"
    /\ UNCHANGED <<chan_mu, waiters, pc_writer, alt_state, in_global,
                   in_local, suspending, wake_pending, running, signal,
                   matched_w, sched_during_suspend>>

Next ==
    \/ \E w \in Writers :
         \/ WriterLockChan(w)
         \/ WriterRegister(w)
         \/ WriterSwitchOut(w)
         \/ WriterDrain(w)
         \/ WriterDrainRelease(w)
         \/ WriterPickedUp(w)
         \/ WriterRun(w)
    \/ ReaderLockChan
    \/ ReaderClaim
    \/ ReaderUnlockChan
    \/ ReaderAltEndScheduleBuggy
    \/ ReaderAltEndRelease

Fairness ==
    /\ \A w \in Writers :
        /\ WF_vars(WriterLockChan(w))
        /\ WF_vars(WriterRegister(w))
        /\ WF_vars(WriterSwitchOut(w))
        /\ WF_vars(WriterDrain(w))
        /\ WF_vars(WriterDrainRelease(w))
        /\ WF_vars(WriterPickedUp(w))
        /\ WF_vars(WriterRun(w))
    /\ WF_vars(ReaderLockChan)
    /\ WF_vars(ReaderClaim)
    /\ WF_vars(ReaderUnlockChan)
    /\ WF_vars(ReaderAltEndScheduleBuggy)
    /\ WF_vars(ReaderAltEndRelease)

Spec == Init /\ [][Next]_vars /\ Fairness

(* SAFETY INVARIANT: schedule must NOT push while the writer is in the
 * suspending_ window. If it does, the imp can be picked up and run on
 * a different worker before its do_switch completes — double-execution.
 *
 * The correct schedule sets wake_pending_ and returns; drain_suspended
 * later pushes. The buggy schedule pushes immediately.
 *
 * TLC expectation: this invariant FAILS — sched_during_suspend is set
 * to TRUE in the buggy path. *)
NoScheduleDuringSuspend ==
    \A w \in Writers : ~sched_during_suspend[w]

====
