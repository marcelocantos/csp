---- MODULE BufferedChanFilter_Bug ----
(*******************************************************************************
 * Bug variant of BufferedChanFilter.
 *
 * The filter's alt loop wedges after delivering the first value: it
 * transitions back to wait_in but FAILS to re-register on in_chan
 * (the FilterRegisterIn action is broken). The remaining writes never
 * reach the filter; downstream readers stay starved.
 *
 * Expected TLC result: EveryReaderServed liveness violation — once one
 * reader is served, no further deliveries are possible.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Readers, NumWrites

ASSUME NumWrites \in Nat
ASSUME NumWrites >= Cardinality(Readers)

VARIABLES
    pc_filter, buf_count, delivered,
    pc_writer, w_done_count,
    pc_reader, r_served,
    in_writer_waiting, in_filter_waiting,
    out_readers_waiting, out_filter_waiting

vars == <<pc_filter, buf_count, delivered, pc_writer, w_done_count,
          pc_reader, r_served, in_writer_waiting, in_filter_waiting,
          out_readers_waiting, out_filter_waiting>>

Init ==
    /\ pc_filter         = "wait_in"
    /\ buf_count         = 0
    /\ delivered         = 0
    /\ pc_writer         = IF NumWrites > 0 THEN "writing" ELSE "done"
    /\ w_done_count      = 0
    /\ pc_reader         = [r \in Readers |-> "want"]
    /\ r_served          = {}
    /\ in_writer_waiting = FALSE
    /\ in_filter_waiting = FALSE
    /\ out_readers_waiting = {}
    /\ out_filter_waiting = FALSE

(* BUG: filter only registers on in once (first iteration). Subsequent
 * transitions back to wait_in fail to re-register, so no more
 * RendezvousIn can fire. *)
FilterRegisterIn ==
    /\ pc_filter = "wait_in"
    /\ buf_count = 0
    /\ ~in_filter_waiting
    /\ delivered = 0        \* BUG: only re-arm on first iteration
    /\ in_filter_waiting' = TRUE
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, pc_reader, r_served, in_writer_waiting,
                   out_readers_waiting, out_filter_waiting>>

FilterRegisterOut ==
    /\ pc_filter = "wait_out"
    /\ buf_count = 1
    /\ ~out_filter_waiting
    /\ out_filter_waiting' = TRUE
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, pc_reader, r_served, in_writer_waiting,
                   in_filter_waiting, out_readers_waiting>>

RendezvousIn ==
    /\ in_writer_waiting
    /\ in_filter_waiting
    /\ pc_writer = "writing"
    /\ in_writer_waiting' = FALSE
    /\ in_filter_waiting' = FALSE
    /\ buf_count' = 1
    /\ pc_filter' = "wait_out"
    /\ w_done_count' = w_done_count + 1
    /\ pc_writer' = IF w_done_count + 1 < NumWrites THEN "writing" ELSE "done"
    /\ UNCHANGED <<delivered, pc_reader, r_served,
                   out_readers_waiting, out_filter_waiting>>

RendezvousOut ==
    /\ out_filter_waiting
    /\ \E r \in out_readers_waiting :
         /\ out_filter_waiting' = FALSE
         /\ out_readers_waiting' = out_readers_waiting \ {r}
         /\ buf_count' = 0
         /\ pc_filter' = "wait_in"
         /\ delivered' = delivered + 1
         /\ r_served' = r_served \cup {r}
         /\ pc_reader' = [pc_reader EXCEPT ![r] = "received"]
    /\ UNCHANGED <<pc_writer, w_done_count,
                   in_writer_waiting, in_filter_waiting>>

WriterRegister ==
    /\ pc_writer = "writing"
    /\ ~in_writer_waiting
    /\ in_writer_waiting' = TRUE
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, pc_reader, r_served,
                   in_filter_waiting, out_readers_waiting, out_filter_waiting>>

ReaderRegister(r) ==
    /\ pc_reader[r] = "want"
    /\ r \notin out_readers_waiting
    /\ out_readers_waiting' = out_readers_waiting \cup {r}
    /\ pc_reader' = [pc_reader EXCEPT ![r] = "registered"]
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, r_served, in_writer_waiting,
                   in_filter_waiting, out_filter_waiting>>

Next ==
    \/ FilterRegisterIn
    \/ FilterRegisterOut
    \/ RendezvousIn
    \/ RendezvousOut
    \/ WriterRegister
    \/ \E r \in Readers : ReaderRegister(r)

Fairness ==
    /\ WF_vars(FilterRegisterIn)
    /\ WF_vars(FilterRegisterOut)
    /\ WF_vars(RendezvousIn)
    /\ WF_vars(RendezvousOut)
    /\ WF_vars(WriterRegister)
    /\ \A r \in Readers : WF_vars(ReaderRegister(r))

Spec == Init /\ [][Next]_vars /\ Fairness

EveryReaderServed ==
    \A r \in Readers : <>(r \in r_served)

====
