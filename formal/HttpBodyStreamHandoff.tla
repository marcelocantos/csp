---- MODULE HttpBodyStreamHandoff ----
(*******************************************************************************
 * Models the HTTP/1.1 server body-streaming handoff from
 * src/http.cc `handle_connection` (🎯T17.5).
 *
 * The orchestrator (connection handler) delivers the request to the user
 * handler as soon as the headers are parsed, then streams body chunks as
 * llhttp's on_body fires.  Each chunk is handed over with a `prialt` that
 * RACES the body push against the handler's response:
 *
 *     switch (prialt(body_ch.w << chunk, resp_ch.r >> resp)) {
 *     case 0:  break;                        // chunk delivered
 *     case 1:  resp_received = true; break;  // early response
 *     case ~0: deliver_body  = false; break; // body_stream dropped
 *     case ~1: goto done;                    // response abandoned
 *     }
 *
 * The race is what makes the connection robust: a handler that rejects the
 * request (401/413) WITHOUT draining its body must not wedge the orchestrator
 * on a push nobody is reading.  Once the handler has responded (or dropped
 * body_stream), the orchestrator keeps draining the body off the wire so the
 * next keep-alive request starts at the right byte boundary.
 *
 * Two safety/liveness claims this spec checks:
 *
 *   (1) Deadlock freedom / termination: from every reachable state the
 *       orchestrator and handler both eventually reach "done" — no handler
 *       behaviour (drain-all, respond-early, drop-body, partial-read) can
 *       wedge the connection.
 *
 *   (2) Keep-alive sync: the orchestrator writes its response only after the
 *       entire body has been consumed off the wire (wire = 0).  Otherwise the
 *       next pipelined request would be mis-parsed.
 *
 * The handler is modelled non-deterministically so TLC explores every
 * interleaving of "read a chunk", "drop the body stream", and "respond".
 ******************************************************************************)

EXTENDS Integers

CONSTANTS NumChunks        \* size of the request body, in stream chunks

ASSUME NumChunks \in Nat

VARIABLES
    wire,        \* body chunks still to be parsed off the wire (0..NumChunks)
    staged,      \* 0|1: a parsed chunk held by the orchestrator, awaiting the race
    orch,        \* "run" | "respond" | "done"
    deliverBody, \* orchestrator still trying to hand body to the handler?
    respRecv,    \* orchestrator has the handler's response in hand?
    handler,     \* "active" | "responded" | "done"
    bodyDropped  \* handler has dropped body_stream (the `case ~0` signal)?

vars == <<wire, staged, orch, deliverBody, respRecv, handler, bodyDropped>>

Init ==
    /\ wire        = NumChunks
    /\ staged      = 0
    /\ orch        = "run"
    /\ deliverBody = TRUE
    /\ respRecv    = FALSE
    /\ handler     = "active"
    /\ bodyDropped = FALSE

(*******************************************************************************
 * ORCHESTRATOR
 ******************************************************************************)

\* Parse the next chunk off the wire into the staging slot.
Parse ==
    /\ orch = "run"
    /\ staged = 0
    /\ wire > 0
    /\ wire' = wire - 1
    /\ staged' = 1
    /\ UNCHANGED <<orch, deliverBody, respRecv, handler, bodyDropped>>

\* prialt case 0: the handler is reading; hand it the staged chunk.
DeliverBody ==
    /\ orch = "run"
    /\ deliverBody
    /\ ~respRecv
    /\ staged = 1
    /\ handler = "active"
    /\ ~bodyDropped
    /\ staged' = 0
    /\ UNCHANGED <<wire, orch, deliverBody, respRecv, handler, bodyDropped>>

\* prialt case ~0: the handler dropped body_stream; stop delivering and drain.
ObserveBodyDrop ==
    /\ orch = "run"
    /\ deliverBody
    /\ ~respRecv
    /\ staged = 1
    /\ bodyDropped
    /\ deliverBody' = FALSE
    /\ UNCHANGED <<wire, staged, orch, respRecv, handler, bodyDropped>>

\* prialt case 1: the handler responded mid-body; take the response and drain.
RaceResp ==
    /\ orch = "run"
    /\ deliverBody
    /\ ~respRecv
    /\ staged = 1
    /\ handler = "active"
    /\ handler' = "responded"
    /\ respRecv' = TRUE
    /\ deliverBody' = FALSE
    /\ staged' = 0                       \* the staged chunk is discarded
    /\ UNCHANGED <<wire, orch, bodyDropped>>

\* Draining: once we are no longer delivering, parsed chunks are discarded.
DiscardStaged ==
    /\ orch = "run"
    /\ ~deliverBody
    /\ staged = 1
    /\ staged' = 0
    /\ UNCHANGED <<wire, orch, deliverBody, respRecv, handler, bodyDropped>>

\* Body fully consumed off the wire: close body_stream, move to the response
\* phase.  This is the ONLY path into "respond", so reaching it implies the
\* whole body has been drained.
ToRespond ==
    /\ orch = "run"
    /\ wire = 0
    /\ staged = 0
    /\ orch' = "respond"
    /\ UNCHANGED <<wire, staged, deliverBody, respRecv, handler, bodyDropped>>

\* Response phase: receive the response if it has not already arrived.
RecvResp ==
    /\ orch = "respond"
    /\ ~respRecv
    /\ handler = "active"
    /\ handler' = "responded"
    /\ respRecv' = TRUE
    /\ UNCHANGED <<wire, staged, orch, deliverBody, bodyDropped>>

\* Write the response and finish.
Finish ==
    /\ orch = "respond"
    /\ respRecv
    /\ orch' = "done"
    /\ UNCHANGED <<wire, staged, deliverBody, respRecv, handler, bodyDropped>>

(*******************************************************************************
 * HANDLER  (non-deterministic: covers every handler archetype)
 ******************************************************************************)

\* Handler chooses to stop reading the body before responding (drop the
\* body_stream reader).  One-shot; only meaningful while still active.
HandlerDropBody ==
    /\ handler = "active"
    /\ ~bodyDropped
    /\ bodyDropped' = TRUE
    /\ UNCHANGED <<wire, staged, orch, deliverBody, respRecv, handler>>

\* Handler finishes once its response has been taken by the orchestrator.
HandlerDone ==
    /\ handler = "responded"
    /\ handler' = "done"
    /\ UNCHANGED <<wire, staged, orch, deliverBody, respRecv, bodyDropped>>

(*******************************************************************************
 * Next-state.  (Reading and responding are the joint rendezvous actions
 * DeliverBody / RaceResp / RecvResp above — the handler's "offer" is folded
 * into those, matching CSP's synchronous rendezvous.)
 ******************************************************************************)

Next ==
    \/ Parse
    \/ DeliverBody
    \/ ObserveBodyDrop
    \/ RaceResp
    \/ DiscardStaged
    \/ ToRespond
    \/ RecvResp
    \/ Finish
    \/ HandlerDropBody
    \/ HandlerDone

Fairness ==
    /\ WF_vars(Parse)
    /\ WF_vars(DeliverBody)
    /\ WF_vars(ObserveBodyDrop)
    /\ WF_vars(DiscardStaged)
    /\ WF_vars(ToRespond)
    /\ WF_vars(RecvResp)
    /\ WF_vars(Finish)
    /\ WF_vars(HandlerDone)
    \* The handler must eventually act on the body or respond; a handler that
    \* does neither forever is a user bug, not a server deadlock.  We require
    \* that responding is always eventually offered (strong fairness), so the
    \* race can resolve even when the handler ignores the body.
    /\ SF_vars(RaceResp)
    /\ SF_vars(RecvResp)

Spec == Init /\ [][Next]_vars /\ Fairness

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ wire \in 0..NumChunks
    /\ staged \in {0, 1}
    /\ orch \in {"run", "respond", "done"}
    /\ deliverBody \in BOOLEAN
    /\ respRecv \in BOOLEAN
    /\ handler \in {"active", "responded", "done"}
    /\ bodyDropped \in BOOLEAN

\* KEEP-ALIVE SYNC: the response phase is reached only with the body fully
\* drained off the wire, so the next pipelined request starts byte-aligned.
KeepAliveSync ==
    (orch \in {"respond", "done"}) => (wire = 0 /\ staged = 0)

\* The orchestrator never writes a response it did not receive.
ResponseBeforeFinish ==
    (orch = "done") => respRecv

\* TERMINATION / DEADLOCK FREEDOM: whatever the handler does, both sides
\* eventually finish.  If any interleaving could wedge the connection, this
\* liveness property fails.
Terminates ==
    <>(orch = "done" /\ handler = "done")

====
