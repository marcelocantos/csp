---- MODULE CloseNotifyDeadlock ----
(*******************************************************************************
 * Models the tls::stream close_notify shutdown handoff from src/tls.cc
 * (🎯T17.3).
 *
 * When either plaintext consumer endpoint of a tls::stream drops, the
 * steady-state stream imp tears down: it sends a best-effort close_notify
 * alert through its ciphertext sink (`up_out`), then drops both upstream
 * endpoints and exits.
 *
 * The hazard is the close_notify SEND.  Two streams wired back-to-back
 * over a pair of unbuffered chan<bytes> transports (the in-process test
 * shape, and any synchronous transport with no send buffer) tear down at
 * roughly the same time.  Each stream, at teardown, tries to push its
 * close_notify into a transport whose consumer — the PEER stream's
 * ciphertext source — has already stopped pulling because the peer is
 * itself tearing down.  Neither alert has a reader.
 *
 *   BUG (blocking send):  `up_out << alert` blocks until a reader appears.
 *       Both streams block on their respective sends, each waiting for the
 *       other to pull, which it never will.  Mutual deadlock — neither imp
 *       ever exits.
 *
 *   FIX (non-blocking send):  `prialt(up_out << alert, csp::none)` tries
 *       once.  If no reader is ready the alert is dropped and the imp
 *       proceeds to exit.  The peer still observes shutdown via the
 *       ciphertext endpoints dropping when the imp exits — exactly the
 *       signal the earlier no-op stub relied on.
 *
 * Each stream is modelled with a phase:
 *
 *   "run"     — doing work; may decide to tear down at any time
 *   "sending" — at the close_notify send point (blocking model only)
 *   "exited"  — dropped its endpoints and returned
 *
 * `pulling[t]` is TRUE while transport t's consumer (the peer stream's
 * ciphertext source) is still pulling ciphertext.  A stream stops pulling
 * the moment it leaves "run" (begins teardown): it will no longer service
 * the transport it consumes from.
 *
 * The liveness claim is simply: both streams eventually exit.  In the bug
 * model TLC finds the interleaving where both reach "sending" with their
 * peers no longer pulling and stay there forever.
 ******************************************************************************)

EXTENDS Integers

(* The two streams.  C consumes from transport c2s' peer... we keep it
 * concrete: stream "C" sends on cs (its up_out) and consumes from sc;
 * stream "S" sends on sc and consumes from cs.  So:
 *   - C's close_notify goes onto transport "cs"; its reader is S.
 *   - S's close_notify goes onto transport "sc"; its reader is C.
 *)
Streams == {"C", "S"}

\* The transport each stream sends its close_notify into.
SinkOf(s)  == IF s = "C" THEN "cs" ELSE "sc"
\* The transport each stream consumes ciphertext from.
SrcOf(s)   == IF s = "C" THEN "sc" ELSE "cs"

VARIABLES
    phase,    \* [Streams -> {"run","sending","exited"}]
    pulling   \* [{"cs","sc"} -> BOOLEAN]: is this transport's consumer pulling?

vars == <<phase, pulling>>

Init ==
    /\ phase   = [s \in Streams |-> "run"]
    /\ pulling = [t \in {"cs","sc"} |-> TRUE]

(*******************************************************************************
 * Common: a stream stops pulling its source the instant it begins teardown.
 ******************************************************************************)
StopPulling(s) == [pulling EXCEPT ![SrcOf(s)] = FALSE]

(*******************************************************************************
 * FIXED MODEL — non-blocking close_notify.
 *
 * Begin teardown: stop pulling the source, then in the SAME step do the
 * best-effort send (succeed if the sink's consumer is currently pulling,
 * else drop) and exit.  Modelling send+exit atomically captures the
 * non-blocking nature: the imp never parks at the send point.
 ******************************************************************************)
TearDownNonBlocking(s) ==
    /\ phase[s] = "run"
    /\ phase'   = [phase EXCEPT ![s] = "exited"]
    /\ pulling' = StopPulling(s)

NextFixed ==
    \E s \in Streams : TearDownNonBlocking(s)

(*******************************************************************************
 * BUG MODEL — blocking close_notify.  (Lives in CloseNotifyDeadlock_Bug.)
 ******************************************************************************)

Next == NextFixed

Fairness == \A s \in Streams : WF_vars(TearDownNonBlocking(s))

Spec == Init /\ [][Next]_vars /\ Fairness

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ phase   \in [Streams -> {"run","sending","exited"}]
    /\ pulling \in [{"cs","sc"} -> BOOLEAN]

\* LIVENESS: both streams eventually exit, no matter the teardown order.
\* In the blocking model this fails: both can wedge at the send point.
BothExit == <>(\A s \in Streams : phase[s] = "exited")

====
