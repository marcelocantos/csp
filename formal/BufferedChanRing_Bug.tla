---- MODULE BufferedChanRing_Bug ----
(*******************************************************************************
 * Bug variant: WriterEnqueue does not wake a waiting reader.
 * Expected TLC: AllReceived violated (or deadlock with fairness).
 ******************************************************************************)

EXTENDS Integers, Sequences

CONSTANTS Cap, NumMsgs

ASSUME Cap \in Nat /\ Cap >= 1
ASSUME NumMsgs \in Nat /\ NumMsgs >= 1

VARIABLES buf, pc_w, pc_r, sent, received, w_waiting, r_waiting, closed_w, closed_r

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

\* BUG: no wake of r_waiting on enqueue.
WriterEnqueue ==
    /\ pc_w = "send"
    /\ ~closed_w /\ ~closed_r
    /\ sent < NumMsgs
    /\ ~BufFull
    /\ buf' = Append(buf, sent)
    /\ sent' = sent + 1
    /\ pc_w' = IF sent + 1 < NumMsgs THEN "send" ELSE "done"
    /\ UNCHANGED <<pc_r, received, w_waiting, r_waiting, closed_w, closed_r>>

WriterWait ==
    /\ pc_w = "send" /\ sent < NumMsgs /\ BufFull /\ ~w_waiting
    /\ w_waiting' = TRUE /\ pc_w' = "waiting"
    /\ UNCHANGED <<buf, pc_r, sent, received, r_waiting, closed_w, closed_r>>

WriterResume ==
    /\ pc_w = "waiting" /\ ~w_waiting
    /\ pc_w' = "send"
    /\ UNCHANGED <<buf, pc_r, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

WriterClose ==
    /\ pc_w \in {"send", "done"} /\ ~closed_w /\ sent = NumMsgs
    /\ closed_w' = TRUE /\ pc_w' = "done"
    /\ IF r_waiting THEN /\ r_waiting' = FALSE /\ pc_r' = "recv"
       ELSE UNCHANGED <<r_waiting, pc_r>>
    /\ UNCHANGED <<buf, sent, received, w_waiting, closed_r>>

ReaderDequeue ==
    /\ pc_r = "recv" /\ ~closed_r /\ ~BufEmpty
    /\ buf' = Tail(buf) /\ received' = received + 1
    /\ IF w_waiting THEN /\ w_waiting' = FALSE /\ pc_w' = "send"
       ELSE UNCHANGED <<w_waiting, pc_w>>
    /\ UNCHANGED <<sent, r_waiting, closed_w, closed_r, pc_r>>

ReaderWait ==
    /\ pc_r = "recv" /\ BufEmpty /\ ~closed_w /\ ~r_waiting
    /\ r_waiting' = TRUE /\ pc_r' = "waiting"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, closed_w, closed_r>>

ReaderResume ==
    /\ pc_r = "waiting" /\ ~r_waiting
    /\ pc_r' = "recv"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

ReaderDoneOnClose ==
    /\ pc_r = "recv" /\ BufEmpty /\ closed_w
    /\ pc_r' = "done"
    /\ UNCHANGED <<buf, pc_w, sent, received, w_waiting, r_waiting, closed_w, closed_r>>

Next ==
    \/ WriterEnqueue \/ WriterWait \/ WriterResume \/ WriterClose
    \/ ReaderDequeue \/ ReaderWait \/ ReaderResume \/ ReaderDoneOnClose

Fairness ==
    /\ WF_vars(WriterEnqueue) /\ WF_vars(WriterWait) /\ WF_vars(WriterResume)
    /\ WF_vars(WriterClose) /\ WF_vars(ReaderDequeue) /\ WF_vars(ReaderWait)
    /\ WF_vars(ReaderResume) /\ WF_vars(ReaderDoneOnClose)

Spec == Init /\ [][Next]_vars /\ Fairness

AllReceived == <>(received = NumMsgs)

====
