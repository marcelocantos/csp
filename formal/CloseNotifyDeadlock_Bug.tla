---- MODULE CloseNotifyDeadlock_Bug ----
(*******************************************************************************
 * BUGGY variant of CloseNotifyDeadlock — the original blocking
 * close_notify send.  See CloseNotifyDeadlock.tla for the full model and
 * the fixed (non-blocking) version.
 *
 * Here the teardown is two-phase, matching `up_out << alert` as a BLOCKING
 * write:
 *
 *   BeginTearDown(s): leave "run", stop pulling s's source, park at the
 *                     close_notify send point ("sending").
 *
 *   CompleteSend(s):  the blocking write can only complete when the sink's
 *                     consumer (the peer stream) is still pulling.  Once
 *                     the peer has begun its own teardown it no longer
 *                     pulls, so this action is permanently disabled and the
 *                     stream is stuck in "sending" forever.
 *
 * TLC violates BothExit via:
 *   BeginTearDown("C") -> BeginTearDown("S")
 * after which neither CompleteSend is enabled (each peer stopped pulling),
 * and both streams are wedged in "sending".
 ******************************************************************************)

EXTENDS Integers

Streams == {"C", "S"}
SinkOf(s)  == IF s = "C" THEN "cs" ELSE "sc"
SrcOf(s)   == IF s = "C" THEN "sc" ELSE "cs"

VARIABLES
    phase,
    pulling

vars == <<phase, pulling>>

Init ==
    /\ phase   = [s \in Streams |-> "run"]
    /\ pulling = [t \in {"cs","sc"} |-> TRUE]

StopPulling(s) == [pulling EXCEPT ![SrcOf(s)] = FALSE]

\* Phase 1: begin teardown — stop servicing the source, park at the send.
BeginTearDown(s) ==
    /\ phase[s] = "run"
    /\ phase'   = [phase EXCEPT ![s] = "sending"]
    /\ pulling' = StopPulling(s)

\* Phase 2: the blocking send completes ONLY if the sink's consumer is
\* still pulling.  SinkOf(s)'s consumer is the peer stream.
CompleteSend(s) ==
    /\ phase[s] = "sending"
    /\ pulling[SinkOf(s)]            \* a reader is ready → write rendezvous
    /\ phase' = [phase EXCEPT ![s] = "exited"]
    /\ UNCHANGED pulling

Next ==
    \E s \in Streams : BeginTearDown(s) \/ CompleteSend(s)

Fairness ==
    \A s \in Streams :
        /\ WF_vars(BeginTearDown(s))
        /\ WF_vars(CompleteSend(s))

Spec == Init /\ [][Next]_vars /\ Fairness

TypeOK ==
    /\ phase   \in [Streams -> {"run","sending","exited"}]
    /\ pulling \in [{"cs","sc"} -> BOOLEAN]

BothExit == <>(\A s \in Streams : phase[s] = "exited")

====
