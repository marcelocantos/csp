---- MODULE WebSocketClose ----
(* One connection, two I/O imps and its lifetime owner. Each imp can be
   blocked on socket readiness or a channel handoff. Cancellation releases
   both kinds of wait; shutdown does not release the descriptor number.
   Only joining both imps allows close() to release/reuse that number.

   The buggy variant reproduces the old reader-owned close while the writer
   can still be inside io::write. This is a finite lifecycle abstraction,
   not a model of TCP, framing, reactor internals or elapsed deadlines.
   Eventual close assumes fair scheduling of awakened imps. Cancellation
   exceptions use synchronous completion sends ("report"); joining must
   drain those sends, not just wait for completion-channel death. *)
EXTENDS TLC
CONSTANTS JoinWriter, ConsumeFailures
VARIABLES stop, cancelled, peerClose, reader, writer, fd
vars == <<stop, cancelled, peerClose, reader, writer, fd>>

Init == /\ stop = FALSE /\ cancelled = FALSE /\ peerClose = FALSE
        /\ reader \in {"io", "channel"}
        /\ writer \in {"io", "channel"}
        /\ fd = "open"

RequestClose == /\ ~stop /\ stop' = TRUE
                /\ UNCHANGED <<cancelled, peerClose, reader, writer, fd>>
PeerClose == /\ ~peerClose /\ peerClose' = TRUE
             /\ UNCHANGED <<stop, cancelled, reader, writer, fd>>
Cancel == /\ stop /\ ~cancelled /\ cancelled' = TRUE
          /\ UNCHANGED <<stop, peerClose, reader, writer, fd>>
ReaderExit == /\ reader \in {"io", "channel"} /\ (cancelled \/ peerClose)
              /\ reader' = IF cancelled THEN "report" ELSE "done"
              /\ UNCHANGED <<stop, cancelled, peerClose, writer, fd>>
WriterExit == /\ writer \in {"io", "channel"} /\ (cancelled \/ peerClose)
              /\ writer' = IF cancelled THEN "report" ELSE "done"
              /\ UNCHANGED <<stop, cancelled, peerClose, reader, fd>>
DrainReader == /\ ConsumeFailures /\ reader = "report" /\ reader' = "done"
               /\ UNCHANGED <<stop, cancelled, peerClose, writer, fd>>
DrainWriter == /\ ConsumeFailures /\ writer = "report" /\ writer' = "done"
               /\ UNCHANGED <<stop, cancelled, peerClose, reader, fd>>
ReleaseFd == /\ fd = "open" /\ reader = "done"
             /\ (~JoinWriter \/ writer = "done")
             /\ fd' = "closed"
             /\ UNCHANGED <<stop, cancelled, peerClose, reader, writer>>

Next == RequestClose \/ PeerClose \/ Cancel \/ ReaderExit \/ WriterExit
        \/ DrainReader \/ DrainWriter \/ ReleaseFd
Spec == Init /\ [][Next]_vars /\ WF_vars(Cancel) /\ WF_vars(ReaderExit)
        /\ WF_vars(WriterExit) /\ WF_vars(ReleaseFd)
        /\ WF_vars(DrainReader) /\ WF_vars(DrainWriter)
TypeOK == /\ stop \in BOOLEAN /\ cancelled \in BOOLEAN /\ peerClose \in BOOLEAN
          /\ reader \in {"io", "channel", "report", "done"}
          /\ writer \in {"io", "channel", "report", "done"}
          /\ fd \in {"open", "closed"}
NoUseAfterClose == fd = "closed" => (reader = "done" /\ writer = "done")
CloseCompletes == stop ~> (fd = "closed")
====
