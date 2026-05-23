---- MODULE BufferedChanFilter ----
(*******************************************************************************
 * Models the buffered-chan filter imp's alt loop from
 * include/csp/csp.h (`chan<T>::chan(size_t capacity)`):
 *
 *     for (;;) {
 *         idx = alt(
 *             buf.full()  ? ~in  : in  >> staging.value,
 *             buf.empty() ? ~out : detail::write_op_for(out, buf.front()));
 *         switch (idx) { case 0: buf.push(); case 1: buf.pop(); ... }
 *     }
 *
 * Motivation: 🎯T26 (paper 27). Multiple successive readers must each
 * be served by the filter without the filter wedging. Web_crawler's
 * frontier is chan<FetchReq>(32) with 1 writer (coordinator) and 4
 * concurrent readers (workers). The hang shape: filter delivers some
 * values, then stops.
 *
 * Model: 1-slot buf, N writes from the producer, M concurrent readers
 * each doing 1 read. We check liveness: every reader eventually gets
 * its value, every write eventually gets delivered.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Readers,        \* set of reader IDs, e.g. {r1, r2}
    NumWrites       \* total writes the producer will perform

ASSUME NumWrites \in Nat
ASSUME NumWrites >= Cardinality(Readers)  \* every reader gets a value

VARIABLES
    pc_filter,           \* "wait_in" | "wait_out" | "done"
    buf_count,           \* 0 | 1
    delivered,           \* total values delivered to any reader
    pc_writer,           \* "writing" | "done"
    w_done_count,        \* writes completed (sent into buf)
    pc_reader,           \* [Reader -> "want" | "registered" | "received"]
    r_served,            \* set of readers who have received a value
    in_writer_waiting,
    in_filter_waiting,
    out_readers_waiting, \* set of readers currently registered as out-chan waiters
    out_filter_waiting

vars == <<pc_filter, buf_count, delivered,
          pc_writer, w_done_count,
          pc_reader, r_served,
          in_writer_waiting, in_filter_waiting,
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

(*******************************************************************************
 * FILTER ACTIONS
 ******************************************************************************)

FilterRegisterIn ==
    /\ pc_filter = "wait_in"
    /\ buf_count = 0
    /\ ~in_filter_waiting
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

(*******************************************************************************
 * RENDEZVOUS
 *
 * In: writer + filter both registered → handoff, buf=1, filter→wait_out.
 * Out: any one registered reader + filter → handoff, buf=0, filter→wait_in.
 ******************************************************************************)

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

(*******************************************************************************
 * WRITER ACTIONS
 ******************************************************************************)

WriterRegister ==
    /\ pc_writer = "writing"
    /\ ~in_writer_waiting
    /\ in_writer_waiting' = TRUE
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, pc_reader, r_served,
                   in_filter_waiting, out_readers_waiting, out_filter_waiting>>

(*******************************************************************************
 * READER ACTIONS
 ******************************************************************************)

ReaderRegister(r) ==
    /\ pc_reader[r] = "want"
    /\ r \notin out_readers_waiting
    /\ out_readers_waiting' = out_readers_waiting \cup {r}
    /\ pc_reader' = [pc_reader EXCEPT ![r] = "registered"]
    /\ UNCHANGED <<pc_filter, buf_count, delivered, pc_writer,
                   w_done_count, r_served, in_writer_waiting,
                   in_filter_waiting, out_filter_waiting>>

(*******************************************************************************
 * Next-state.
 ******************************************************************************)

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

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ pc_filter \in {"wait_in", "wait_out", "done"}
    /\ buf_count \in {0, 1}
    /\ delivered \in 0..NumWrites
    /\ pc_writer \in {"writing", "done"}
    /\ w_done_count \in 0..NumWrites
    /\ pc_reader \in [Readers -> {"want", "registered", "received"}]
    /\ r_served \subseteq Readers
    /\ in_writer_waiting \in BOOLEAN
    /\ in_filter_waiting \in BOOLEAN
    /\ out_readers_waiting \subseteq Readers
    /\ out_filter_waiting \in BOOLEAN

ConservationOfValues == w_done_count = delivered + buf_count

FilterStateConsistent ==
    /\ (pc_filter = "wait_in")  => (buf_count = 0)
    /\ (pc_filter = "wait_out") => (buf_count = 1)

(* THE KEY LIVENESS PROPERTY: every reader is eventually served. If the
 * filter wedges between deliveries, this fails. *)
EveryReaderServed ==
    \A r \in Readers : <>(r \in r_served)

(* Total throughput: at least Cardinality(Readers) values delivered. *)
EnoughDelivered ==
    <>(delivered >= Cardinality(Readers))

====
