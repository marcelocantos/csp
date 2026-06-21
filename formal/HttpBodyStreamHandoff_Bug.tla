---- MODULE HttpBodyStreamHandoff_Bug ----
(*******************************************************************************
 * BUGGY variant of HttpBodyStreamHandoff: the NAIVE body-streaming design
 * that delivers each body chunk with a plain BLOCKING push and only receives
 * the handler's response in a dedicated response phase AFTER the whole body
 * has been streamed:
 *
 *     for (chunk : body) body_ch.w << chunk;   // blocking, no race
 *     resp_ch.r >> resp;                        // only after the body
 *
 * This is the design 🎯T17.5 deliberately avoids.  A handler that rejects the
 * request without draining its body (an early 401 / 413) wedges the
 * connection: the orchestrator blocks forever pushing a chunk nobody reads,
 * while the handler blocks forever offering a response nobody receives.
 *
 * The handler non-deterministically commits to one of two archetypes:
 *   - "drain":  reads the whole body, then responds   (terminates fine)
 *   - "reject": responds without reading the body      (DEADLOCKS here)
 *
 * Expected result: TLC reports the "reject" behaviour as a violation of the
 * Terminates liveness property (the connection never finishes).
 ******************************************************************************)

EXTENDS Integers

CONSTANTS NumChunks

ASSUME NumChunks \in Nat
ASSUME NumChunks > 0          \* a body must exist for the deadlock to bite

VARIABLES
    wire,        \* body chunks still on the wire
    staged,      \* 0|1: parsed chunk awaiting a blocking push
    orch,        \* "run" | "respond" | "done"
    respRecv,    \* orchestrator has the response?
    handler,     \* "active" | "responded" | "done"
    intent       \* "undecided" | "drain" | "reject"

vars == <<wire, staged, orch, respRecv, handler, intent>>

Init ==
    /\ wire     = NumChunks
    /\ staged   = 0
    /\ orch     = "run"
    /\ respRecv = FALSE
    /\ handler  = "active"
    /\ intent   = "undecided"

\* The handler commits to an archetype.
Decide ==
    /\ handler = "active"
    /\ intent = "undecided"
    /\ intent' \in {"drain", "reject"}
    /\ UNCHANGED <<wire, staged, orch, respRecv, handler>>

Parse ==
    /\ orch = "run"
    /\ staged = 0
    /\ wire > 0
    /\ wire' = wire - 1
    /\ staged' = 1
    /\ UNCHANGED <<orch, respRecv, handler, intent>>

\* Blocking push: only a draining handler ever reads, so this is the only
\* way staged can clear — and a rejecting handler never enables it.
ReadChunk ==
    /\ orch = "run"
    /\ staged = 1
    /\ handler = "active"
    /\ intent = "drain"
    /\ staged' = 0
    /\ UNCHANGED <<wire, orch, respRecv, handler, intent>>

ToRespond ==
    /\ orch = "run"
    /\ wire = 0
    /\ staged = 0
    /\ orch' = "respond"
    /\ UNCHANGED <<wire, staged, respRecv, handler, intent>>

\* The response is received ONLY in the dedicated phase — there is no race
\* during streaming.  This is the missing escape hatch.
RecvResp ==
    /\ orch = "respond"
    /\ ~respRecv
    /\ handler = "active"
    /\ intent \in {"drain", "reject"}
    /\ handler' = "responded"
    /\ respRecv' = TRUE
    /\ UNCHANGED <<wire, staged, orch, intent>>

Finish ==
    /\ orch = "respond"
    /\ respRecv
    /\ orch' = "done"
    /\ UNCHANGED <<wire, staged, respRecv, handler, intent>>

HandlerDone ==
    /\ handler = "responded"
    /\ handler' = "done"
    /\ UNCHANGED <<wire, staged, orch, respRecv, intent>>

Next ==
    \/ Decide
    \/ Parse
    \/ ReadChunk
    \/ ToRespond
    \/ RecvResp
    \/ Finish
    \/ HandlerDone

Fairness ==
    /\ WF_vars(Decide)
    /\ WF_vars(Parse)
    /\ WF_vars(ReadChunk)
    /\ WF_vars(ToRespond)
    /\ WF_vars(RecvResp)
    /\ WF_vars(Finish)
    /\ WF_vars(HandlerDone)

Spec == Init /\ [][Next]_vars /\ Fairness

TypeOK ==
    /\ wire \in 0..NumChunks
    /\ staged \in {0, 1}
    /\ orch \in {"run", "respond", "done"}
    /\ respRecv \in BOOLEAN
    /\ handler \in {"active", "responded", "done"}
    /\ intent \in {"undecided", "drain", "reject"}

\* The same liveness property as the fixed spec.  The "reject" handler
\* violates it: the connection never terminates.
Terminates ==
    <>(orch = "done" /\ handler = "done")

====
