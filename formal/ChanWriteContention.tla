---- MODULE ChanWriteContention ----
(*******************************************************************************
 * Models N writers concurrently rendezvousing with 1 reader on a single
 * unbuffered channel, with full Imp::schedule short-circuit logic and
 * drain_suspended interaction.
 *
 * Motivation: 🎯T26 (paper 27). The web_crawler example hangs on Linux
 * when 4 worker imps all attempt `out << CrawlResult` against an
 * unbuffered `results` channel that the coordinator reads. Wake-all
 * unpark_one variant ruled out the worker-wake granularity, leaving
 * "missed make_runnable upstream" or "chan_op rendezvous lost a peer
 * schedule" as the remaining hypotheses.
 *
 * This spec models the chan_op rendezvous of N writers + 1 reader, and
 * the schedule() short-circuits on in_global_ / next_ / suspending_. The
 * key safety property is the NoLostWriter invariant: every writer that
 * has rendezvoused (alt_state = claimed, signal set) must eventually be
 * scheduled to run again (in_global_ or running).
 *
 * Participants:
 *   Writers — N data-writer imps doing `out << val` on the channel
 *   Reader  — 1 imp doing `in >> val` on the channel; matches one writer
 *
 * Reader behaviour models alt_end: after matching writer W under
 * chan_mu, unlock, then call W->make_runnable(), which calls
 * schedule(W). Schedule walks the short-circuits.
 *
 * Code references (src/channel.cc, src/csp.cc):
 *   WriterRegister     — channel.cc:430-456 (prialt_begin Phase 2)
 *   WriterSwitchOut    — channel.cc:457-459 (unlock + do_switch)
 *   ReaderScanMatch    — channel.cc:298-348 (Phase 1 scan + CAS claim)
 *   ReaderAltEnd       — channel.cc:493-504 (peer->make_runnable)
 *   ScheduleEnter      — csp.cc:183-208  (Imp::schedule)
 *   ScheduleSCInGlobal — csp.cc:191      (in_global_ short-circuit)
 *   ScheduleSCSuspend  — csp.cc:199-202  (suspending_ short-circuit, sets wake_pending_)
 *   ScheduleSCInLocal  — csp.cc:205      (next_ short-circuit)
 *   SchedulePush       — csp.cc:206      (push_to_global)
 *   DrainSuspended     — csp.cc:95-116   (clear suspending, drain wake_pending_)
 *
 * The spec abstracts away worker park/unpark — wake-all already ruled
 * out the worker-wake granularity. We track the writer's "ready to run"
 * state as in_global_ \/ in_local_; an explicit "scheduled to run" is
 * sufficient liveness evidence.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Writers       \* set of writer IDs, e.g. {w1, w2}

(*******************************************************************************
 * STATE
 *
 * Channel: one unbuffered channel, mutex + waiter set.
 *   chan_mu — "none" | "reader" | writer w  (whoever holds the lock)
 *   waiters — set of writers registered as waiting on the channel
 *
 * Per-writer state (mapping from writer to value):
 *   pc_writer    — "start" | "lock_chan" | "registered" | "suspending"
 *                  | "switched_out" | "drained" | "woken" | "running" | "done"
 *   alt_state    — "idle" | "waiting" | "claimed"
 *   in_global    — writer is in the global run queue
 *   in_local     — writer is in some worker's local run queue
 *   suspending   — writer is in the unlock_all → drain_suspended window
 *   wake_pending — deferred wake set by schedule() during suspending_ window
 *   running      — writer is currently executing on a worker
 *   signal       — match index set by the matching reader (~0 if matched)
 *
 * Reader state:
 *   pc_reader — "start" | "lock_chan" | "scanning" | "matched"
 *               | "unlock_chan" | "altend" | "done"
 *   matched_w — which writer the reader matched (or "none")
 *
 * Global scheduler:
 *   global_mu — "none" | "reader" | "drain_" w | "sched_" w
 *               (who holds Runtime::global_mu)
 *******************************************************************************)

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
    global_mu

vars == <<chan_mu, waiters, pc_writer, alt_state, in_global, in_local,
          suspending, wake_pending, running, signal,
          pc_reader, matched_w, global_mu>>

(*******************************************************************************
 * Init
 ******************************************************************************)
Init ==
    /\ chan_mu      = "none"
    /\ waiters      = {}
    /\ pc_writer    = [w \in Writers |-> "start"]
    /\ alt_state    = [w \in Writers |-> "idle"]
    /\ in_global    = [w \in Writers |-> FALSE]
    /\ in_local     = [w \in Writers |-> FALSE]
    /\ suspending   = [w \in Writers |-> FALSE]
    /\ wake_pending = [w \in Writers |-> FALSE]
    /\ running      = [w \in Writers |-> TRUE]  \* writers are initially running on their workers
    /\ signal       = [w \in Writers |-> "none"]
    /\ pc_reader    = "start"
    /\ matched_w    = "none"
    /\ global_mu    = "none"

(*******************************************************************************
 * WRITER ACTIONS
 *
 * A writer enters prialt with a single chanop (the write to the channel).
 * It locks chan_mu, registers itself, sets alt_state=WAITING, then
 * begins the suspend protocol: suspending_=TRUE under chan_mu, unlock,
 * switch out, drain.
 ******************************************************************************)

(* Acquire chan_mu. Models lock_all() for a single channel.
 * TLA:ChanWriteContention.WriterLockChan *)
WriterLockChan(w) ==
    /\ pc_writer[w] = "start"
    /\ chan_mu = "none"
    /\ chan_mu' = w
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "lock_chan"]
    /\ UNCHANGED <<waiters, alt_state, in_global, in_local, suspending,
                   wake_pending, running, signal, pc_reader, matched_w,
                   global_mu>>

(* Phase 2 register: add to waiters, set alt_state=WAITING.
 * Then mark suspending=TRUE while still under chan_mu (so a
 * concurrent reader that proceeds after unlock_chan will see
 * suspending=TRUE on this writer if it tries schedule()).
 * TLA:ChanWriteContention.WriterRegister *)
WriterRegister(w) ==
    /\ pc_writer[w] = "lock_chan"
    /\ chan_mu = w
    /\ waiters' = waiters \cup {w}
    /\ alt_state' = [alt_state EXCEPT ![w] = "waiting"]
    /\ suspending' = [suspending EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "registered"]
    /\ UNCHANGED <<chan_mu, in_global, in_local, wake_pending,
                   running, signal, pc_reader, matched_w, global_mu>>

(* Unlock chan_mu and "switch out" — running becomes FALSE.
 * Maps to channel.cc:457-458: unlock_all(); do_switch(detach).
 * After this step, the writer is suspended; any wake must come via
 * schedule().
 * TLA:ChanWriteContention.WriterSwitchOut *)
WriterSwitchOut(w) ==
    /\ pc_writer[w] = "registered"
    /\ chan_mu = w
    /\ chan_mu' = "none"
    /\ running' = [running EXCEPT ![w] = FALSE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "switched_out"]
    /\ UNCHANGED <<waiters, alt_state, in_global, in_local, suspending,
                   wake_pending, signal, pc_reader, matched_w, global_mu>>

(* drain_suspended runs on the destination context's side, under
 * global_mu. Clear suspending; if wake_pending was set, push to
 * global. Models csp.cc:95-116.
 * TLA:ChanWriteContention.WriterDrain *)
WriterDrain(w) ==
    /\ pc_writer[w] = "switched_out"
    /\ global_mu = "none"
    /\ global_mu' = "drain"  \* (we'll just use a fresh tag)
    /\ suspending' = [suspending EXCEPT ![w] = FALSE]
    /\ IF wake_pending[w] /\ ~in_global[w]
       THEN /\ wake_pending' = [wake_pending EXCEPT ![w] = FALSE]
            /\ in_global' = [in_global EXCEPT ![w] = TRUE]
       ELSE /\ wake_pending' = [wake_pending EXCEPT ![w] = FALSE]
            /\ UNCHANGED in_global
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "drained"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_local, running, signal,
                   pc_reader, matched_w>>

(* Release global_mu after drain. *)
WriterDrainRelease(w) ==
    /\ pc_writer[w] = "drained"
    /\ global_mu = "drain"
    /\ global_mu' = "none"
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "asleep"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   pc_reader, matched_w>>

(* The writer becomes runnable once in_global=TRUE (worker pulls it).
 * Models take_from_global → schedule_local → local_next → run.
 * TLA:ChanWriteContention.WriterRunnable *)
WriterPickedUp(w) ==
    /\ pc_writer[w] \in {"asleep", "drained"}
    /\ alt_state[w] = "claimed"  \* must have been matched
    /\ in_global[w]
    /\ in_global' = [in_global EXCEPT ![w] = FALSE]
    /\ in_local' = [in_local EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "woken"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, suspending, wake_pending,
                   running, signal, pc_reader, matched_w, global_mu>>

(* Writer runs: alt_end-style cleanup. Removes from local queue, marks running.
 * TLA:ChanWriteContention.WriterRun *)
WriterRun(w) ==
    /\ pc_writer[w] = "woken"
    /\ in_local[w]
    /\ in_local' = [in_local EXCEPT ![w] = FALSE]
    /\ running' = [running EXCEPT ![w] = TRUE]
    /\ pc_writer' = [pc_writer EXCEPT ![w] = "done"]
    /\ UNCHANGED <<chan_mu, waiters, alt_state, in_global, suspending,
                   wake_pending, signal, pc_reader, matched_w, global_mu>>

(*******************************************************************************
 * READER ACTIONS
 *
 * Reader enters prialt with a single read chanop. Locks chan_mu, scans
 * waiters, finds one to claim, CAS WAITING→CLAIMED on that writer,
 * sets signal, unlocks, then alt_end calls schedule(writer).
 *
 * Reader does NOT suspend in this model — it has a match available
 * immediately (the contention case we're modelling).
 ******************************************************************************)

(* Reader acquires chan_mu.
 * TLA:ChanWriteContention.ReaderLockChan *)
ReaderLockChan ==
    /\ pc_reader = "start"
    /\ chan_mu = "none"
    /\ chan_mu' = "reader"
    /\ pc_reader' = "scanning"
    /\ UNCHANGED <<waiters, pc_writer, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   matched_w, global_mu>>

(* Scan waiters, pick any one with alt_state=WAITING, CAS to CLAIMED,
 * set signal. Models the Phase 1 scan match in channel.cc:298-348.
 * TLA:ChanWriteContention.ReaderClaim *)
ReaderClaim ==
    /\ pc_reader = "scanning"
    /\ chan_mu = "reader"
    /\ \E w \in waiters :
        /\ alt_state[w] = "waiting"
        /\ alt_state' = [alt_state EXCEPT ![w] = "claimed"]
        /\ signal' = [signal EXCEPT ![w] = "matched"]
        /\ matched_w' = w
        /\ waiters' = waiters \ {w}  \* claimed waiter is delisted
    /\ pc_reader' = "matched"
    /\ UNCHANGED <<chan_mu, pc_writer, in_global, in_local, suspending,
                   wake_pending, running, global_mu>>

(* Release chan_mu and proceed to alt_end (peer wake).
 * TLA:ChanWriteContention.ReaderUnlockChan *)
ReaderUnlockChan ==
    /\ pc_reader = "matched"
    /\ chan_mu = "reader"
    /\ chan_mu' = "none"
    /\ pc_reader' = "altend"
    /\ UNCHANGED <<waiters, pc_writer, alt_state, in_global, in_local,
                   suspending, wake_pending, running, signal,
                   matched_w, global_mu>>

(* alt_end calls peer->make_runnable(), which calls schedule(peer).
 * Schedule acquires global_mu and walks the short-circuit ladder:
 *   if in_global_   : return  (peer already queued)
 *   if suspending_  : wake_pending_ = TRUE, return  (peer mid-switch)
 *   if next_  (in_local_) : return  (peer in some local queue)
 *   else: push_to_global, set in_global_ = TRUE
 * Models csp.cc:183-208 atomically (full lock-protected critical section).
 * TLA:ChanWriteContention.ReaderAltEndSchedule *)
ReaderAltEndSchedule ==
    /\ pc_reader = "altend"
    /\ global_mu = "none"
    /\ matched_w /= "none"
    /\ LET w == matched_w IN
        /\ global_mu' = "sched"
        /\ IF in_global[w]
           THEN UNCHANGED <<in_global, wake_pending>>  \* short-circuit (a)
           ELSE IF suspending[w]
                THEN /\ wake_pending' = [wake_pending EXCEPT ![w] = TRUE]
                     /\ UNCHANGED in_global
                ELSE IF in_local[w]
                     THEN UNCHANGED <<in_global, wake_pending>>  \* short-circuit (c)
                     ELSE /\ in_global' = [in_global EXCEPT ![w] = TRUE]
                          /\ UNCHANGED wake_pending
    /\ pc_reader' = "altend_release"
    /\ UNCHANGED <<chan_mu, waiters, pc_writer, alt_state, in_local,
                   suspending, running, signal, matched_w>>

(* Release global_mu after schedule(). *)
ReaderAltEndRelease ==
    /\ pc_reader = "altend_release"
    /\ global_mu = "sched"
    /\ global_mu' = "none"
    /\ pc_reader' = "done"
    /\ UNCHANGED <<chan_mu, waiters, pc_writer, alt_state, in_global,
                   in_local, suspending, wake_pending, running, signal,
                   matched_w>>

(*******************************************************************************
 * Next-state relation
 ******************************************************************************)

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
    \/ ReaderAltEndSchedule
    \/ ReaderAltEndRelease

(* Weak fairness on every action: any continuously enabled action
 * eventually fires. With this, TLC's liveness checks find real lost
 * wakeups, not trivial stuttering. *)
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
    /\ WF_vars(ReaderAltEndSchedule)
    /\ WF_vars(ReaderAltEndRelease)

Spec == Init /\ [][Next]_vars /\ Fairness

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

WriterPCs == {"start", "lock_chan", "registered", "switched_out",
              "drained", "asleep", "woken", "done"}
ReaderPCs == {"start", "scanning", "matched", "altend",
              "altend_release", "done"}

TypeOK ==
    /\ chan_mu \in ({"none", "reader"} \cup Writers)
    /\ waiters \subseteq Writers
    /\ pc_writer \in [Writers -> WriterPCs]
    /\ alt_state \in [Writers -> {"idle", "waiting", "claimed"}]
    /\ in_global \in [Writers -> BOOLEAN]
    /\ in_local \in [Writers -> BOOLEAN]
    /\ suspending \in [Writers -> BOOLEAN]
    /\ wake_pending \in [Writers -> BOOLEAN]
    /\ running \in [Writers -> BOOLEAN]
    /\ signal \in [Writers -> {"none", "matched"}]
    /\ pc_reader \in ReaderPCs
    /\ matched_w \in ({"none"} \cup Writers)

(* Safety: at most one writer is matched per reader scan. *)
AtMostOneMatch ==
    \A w1 \in Writers : \A w2 \in Writers :
        (w1 /= w2) =>
            ~(alt_state[w1] = "claimed" /\ alt_state[w2] = "claimed")

(* Safety: signal is set only on claimed writers. *)
SignalImpliesClaimed ==
    \A w \in Writers :
        (signal[w] = "matched") => (alt_state[w] = "claimed")

(* THE KEY INVARIANT: NoLostWriter.
 *
 * If the reader has completed (done), the matched writer has rendezvoused
 * (alt_state = claimed, signal = matched), AND the writer has completed
 * the suspending → drained transition, THEN the writer must eventually
 * become runnable. We assert the safety face of this: the writer is
 * either currently runnable (in_global \/ in_local \/ running) OR there
 * is still some action pending that will make it so (pc_writer not yet
 * past "drained").
 *
 * If TLC finds a state where the reader is done, the writer is matched,
 * the writer is past drained, AND the writer is NOT runnable AND has no
 * pending action — that's the missed-wake bug. *)
NoLostWriter ==
    LET matched_writer_lost(w) ==
        /\ pc_reader = "done"
        /\ alt_state[w] = "claimed"
        /\ pc_writer[w] = "asleep"
        /\ ~in_global[w]
        /\ ~in_local[w]
        /\ ~running[w]
        /\ ~suspending[w]
        /\ ~wake_pending[w]
    IN \A w \in Writers : ~matched_writer_lost(w)

(* Liveness: every matched writer eventually completes (gets to "done"). *)
EveryMatchedWriterCompletes ==
    \A w \in Writers :
        (alt_state[w] = "claimed") ~> (pc_writer[w] = "done")

====
