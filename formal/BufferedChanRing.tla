---- MODULE BufferedChanRing ----
(*******************************************************************************
 * Models paper 33 O4 / 🎯T35: a ring buffer owned by the Channel itself
 * (Go-hchan style), replacing the filter-imp that turns every buffered
 * element into two rendezvous + a third schedulable entity.
 *
 * Under channel mu_:
 *   Send (writer):
 *     if buffer not full: enqueue, wake one waiting reader if any, done
 *     else: register as writer waiter, sleep
 *   Recv (reader):
 *     if buffer not empty: dequeue, wake one waiting writer if any, done
 *     else: register as reader waiter, sleep
 *   Direct rendezvous still possible when opposite waiter is present
 *   even if the buffer is empty/full (optional fast path; this model
 *   prefers the buffer when space/data exists).
 *
 * Participants: 1 writer, 1 reader, capacity Cap slots.
 * Safety: conservation of values, no send into full, no recv from empty,
 *         no lost waiter on close.
 ******************************************************************************)

EXTENDS Integers, FiniteSets, Sequences

CONSTANTS Cap, NumMsgs

ASSUME Cap \in Nat /\ Cap >= 1
ASSUME NumMsgs \in Nat /\ NumMsgs >= 1

VARIABLES
    buf,              \* Sequence of buffered values (length 0..Cap)
    pc_w,             \* writer: "send" | "waiting" | "done"
    pc_r,             \* reader: "recv" | "waiting" | "done"
    sent,             \* values the writer has successfully handed off
    received,         \* values the reader has taken
    w_waiting,        \* writer registered as waiter
    r_waiting,        \* reader registered as waiter
    closed_w,         \* writer endpoint dead
    closed_r          \* reader endpoint dead

vars == <<buf, pc_w, pc_r, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

Init ==
    /\ buf = << >>
    /\ pc_w = "send"
    /\ pc_r = "recv"
    /\ sent = 0
    /\ received = 0
    /\ w_waiting = FALSE
    /\ r_waiting = FALSE
    /\ closed_w = FALSE
    /\ closed_r = FALSE

BufCount == Len(buf)
BufFull  == BufCount = Cap
BufEmpty == BufCount = 0

(*******************************************************************************
 * WRITER
 ******************************************************************************)

\* Enqueue into free slot; wake a waiting reader if present.
WriterEnqueue ==
    /\ pc_w = "send"
    /\ ~closed_w
    /\ ~closed_r
    /\ sent < NumMsgs
    /\ ~BufFull
    /\ buf' = Append(buf, sent)       \* value = sent index
    /\ sent' = sent + 1
    /\ IF r_waiting
       THEN /\ r_waiting' = FALSE
            /\ pc_r' = "recv"         \* reader claimed by buffer edge
       ELSE /\ UNCHANGED <<r_waiting, pc_r>>
    /\ pc_w' = IF sent + 1 < NumMsgs THEN "send" ELSE "done"
    /\ UNCHANGED <<w_waiting, closed_w, closed_r, received>>

\* Buffer full: register and wait.
WriterWait ==
    /\ pc_w = "send"
    /\ ~closed_w
    /\ sent < NumMsgs
    /\ BufFull
    /\ ~w_waiting
    /\ w_waiting' = TRUE
    /\ pc_w' = "waiting"
    /\ UNCHANGED <<buf, pc_r, sent, received, r_waiting, closed_w, closed_r>>

\* Reader dequeued and woke us: resume sending.
WriterResume ==
    /\ pc_w = "waiting"
    /\ ~w_waiting          \* cleared by reader dequeue
    /\ pc_w' = "send"
    /\ UNCHANGED <<buf, pc_r, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

WriterClose ==
    /\ pc_w \in {"send", "done"}
    /\ ~closed_w
    /\ sent = NumMsgs
    /\ closed_w' = TRUE
    /\ pc_w' = "done"
    \* Wake waiting reader so it can observe close after drain.
    /\ IF r_waiting
       THEN /\ r_waiting' = FALSE
            /\ pc_r' = "recv"
       ELSE UNCHANGED <<r_waiting, pc_r>>
    /\ UNCHANGED <<buf, sent, received, w_waiting, closed_r>>

(*******************************************************************************
 * READER
 ******************************************************************************)

ReaderDequeue ==
    /\ pc_r = "recv"
    /\ ~closed_r
    /\ ~BufEmpty
    /\ buf' = Tail(buf)
    /\ received' = received + 1
    /\ IF w_waiting
       THEN /\ w_waiting' = FALSE
            /\ pc_w' = "send"
       ELSE UNCHANGED <<w_waiting, pc_w>>
    /\ pc_r' = "recv"
    /\ UNCHANGED <<sent, r_waiting, closed_w, closed_r>>

ReaderWait ==
    /\ pc_r = "recv"
    /\ ~closed_r
    /\ BufEmpty
    /\ ~closed_w          \* if writer closed and empty → done (below)
    /\ ~r_waiting
    /\ r_waiting' = TRUE
    /\ pc_r' = "waiting"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, closed_w, closed_r>>

ReaderResume ==
    /\ pc_r = "waiting"
    /\ ~r_waiting
    /\ pc_r' = "recv"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

\* Writer closed and buffer drained → reader done.
ReaderDoneOnClose ==
    /\ pc_r = "recv"
    /\ BufEmpty
    /\ closed_w
    /\ pc_r' = "done"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

(*******************************************************************************
 * SPEC
 ******************************************************************************)

Next ==
    \/ WriterEnqueue
    \/ WriterWait
    \/ WriterResume
    \/ WriterClose
    \/ ReaderDequeue
    \/ ReaderWait
    \/ ReaderResume
    \/ ReaderDoneOnClose

Fairness ==
    /\ WF_vars(WriterEnqueue)
    /\ WF_vars(WriterWait)
    /\ WF_vars(WriterResume)
    /\ WF_vars(WriterClose)
    /\ WF_vars(ReaderDequeue)
    /\ WF_vars(ReaderWait)
    /\ WF_vars(ReaderResume)
    /\ WF_vars(ReaderDoneOnClose)

Spec == Init /\ [][Next]_vars /\ Fairness

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ BufCount \in 0..Cap
    /\ sent \in 0..NumMsgs
    /\ received \in 0..NumMsgs
    /\ pc_w \in {"send", "waiting", "done"}
    /\ pc_r \in {"recv", "waiting", "done"}
    /\ w_waiting \in BOOLEAN
    /\ r_waiting \in BOOLEAN
    /\ closed_w \in BOOLEAN
    /\ closed_r \in BOOLEAN

\* Values conserved: sent = received + buffered.
Conservation == sent = received + BufCount

\* Never enqueue past capacity.
NeverOverfill == BufCount <= Cap

\* A waiting writer implies buffer is full (or racing resume).
WriterWaitImpliesFull ==
    (pc_w = "waiting" /\ w_waiting) => BufFull

\* A waiting reader implies buffer is empty.
ReaderWaitImpliesEmpty ==
    (pc_r = "waiting" /\ r_waiting) => BufEmpty

\* Liveness: every message is eventually received.
AllReceived == <>(received = NumMsgs)

====
