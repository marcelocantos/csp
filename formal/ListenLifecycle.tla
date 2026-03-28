---- MODULE ListenLifecycle ----
(*******************************************************************************
 * Models the net::listen accept loop lifecycle to find channel leaks.
 *
 * The protocol involves 5 imps and several shared resources.  The invariant
 * is that after all imps exit, all channels are released (refcount 0).
 *
 * Participants:
 *   Producer    — owns cancel_guard, runs accept loop, creates connections
 *   Sentinel    — watches connections reader death via out_copy writer
 *   Server      — holds connections reader, reads one connection, exits
 *   ByteReader  — reads from fd, writes to byte channel (part of connection)
 *   ByteWriter  — reads from byte channel, writes to fd (part of connection)
 *
 * Channels (modeled as refcounts for writer/reader endpoints):
 *   conn_w, conn_r    — connections channel (spawn_producer creates)
 *   cancel_w, cancel_r — cancel_state's trigger/signal pair
 *   byte_in_w, byte_in_r   — byte_reader's output channel
 *   byte_out_w, byte_out_r — byte_writer's input channel
 *   spawn_prod_r      — producer's spawn handle (reader, discarded)
 *   spawn_sent_r      — sentinel's spawn handle (reader, discarded)
 *   spawn_br_r        — byte_reader's spawn handle
 *   spawn_bw_r        — byte_writer's spawn handle
 *
 * The cancel_guard holds a csp::local binding that references the
 * cancel_state in the HAMT.  When the guard is destroyed, the binding
 * restores the previous value — but only correctly if destroyed in
 * imp context.
 *
 * Code references (src/net.cc, src/cancel.cc):
 *   Producer creates cancel_guard (cancellation())
 *   cancel_guard::impl has csp::local binding → HAMT holds cancel_state
 *   Sentinel holds shared_ptr<cancel_guard> copy
 *   cancel_guard destroyed when last shared_ptr drops
 *   csp::local destructor runs wherever shared_ptr destructor runs
 *
 * Simplifications:
 *   - Channel operations (prialt, read, write) are abstracted away
 *   - Focus is on resource lifecycle (acquire/release), not data flow
 *   - HAMT is modeled as a boolean "holds cancel_state" flag
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS NULL

VARIABLES
    \* Imp states: "running", "exiting", "dead"
    producer, sentinel, server, byte_reader, byte_writer,

    \* Channel endpoint refcounts (writers, readers)
    conn_w,       \* connections channel writer refs
    conn_r,       \* connections channel reader refs
    cancel_w,     \* cancel_state trigger writer refs
    cancel_r,     \* cancel_state reader refs
    byte_in_w,    \* byte_reader output writer refs
    byte_in_r,    \* byte_reader output reader refs
    byte_out_w,   \* byte_writer input writer refs
    byte_out_r,   \* byte_writer input reader refs

    \* cancel_guard shared_ptr refcount
    guard_refs,

    \* HAMT holds cancel_state (via csp::local binding)
    hamt_holds_cancel,

    \* Whether csp::local was properly restored (destroyed in imp context)
    local_restored,

    \* Who last destroyed the guard (for diagnosing the bug)
    guard_destroyed_by

vars == <<producer, sentinel, server, byte_reader, byte_writer,
          conn_w, conn_r, cancel_w, cancel_r,
          byte_in_w, byte_in_r, byte_out_w, byte_out_r,
          guard_refs, hamt_holds_cancel, local_restored,
          guard_destroyed_by>>

-----------------------------------------------------------------------------
(* Initial state:
   - spawn_producer creates connections channel
   - Producer starts running, creates cancel_guard (which creates cancel channel
     and binds to HAMT)
   - Sentinel starts with shared_ptr copy of guard + out_copy (conn writer)
   - Server starts with connections reader
   - No connection yet (byte_reader/byte_writer not started) *)

Init ==
    /\ producer = "running"
    /\ sentinel = "running"
    /\ server = "running"
    /\ byte_reader = "waiting"      \* not yet started
    /\ byte_writer = "waiting"      \* not yet started
    \* connections channel: producer has writer, out_copy for sentinel,
    \* server has reader
    /\ conn_w = 2                   \* producer's out + sentinel's out_copy
    /\ conn_r = 1                   \* server's connections reader
    \* cancel_state channel: created by cancel_guard
    /\ cancel_w = 1                 \* trigger writer
    /\ cancel_r = 1                 \* signal reader
    \* byte channels: not yet created
    /\ byte_in_w = 0
    /\ byte_in_r = 0
    /\ byte_out_w = 0
    /\ byte_out_r = 0
    \* cancel_guard: producer + sentinel each hold shared_ptr
    /\ guard_refs = 2
    \* HAMT holds cancel_state via csp::local
    /\ hamt_holds_cancel = TRUE
    /\ local_restored = FALSE
    /\ guard_destroyed_by = NULL

-----------------------------------------------------------------------------
(* Server accepts a connection → byte_reader and byte_writer start *)

ServerAccept ==
    /\ server = "running"
    /\ byte_reader = "waiting"
    /\ byte_reader' = "running"
    /\ byte_writer' = "running"
    \* Connection creates two byte channels
    /\ byte_in_w' = 1              \* byte_reader holds writer
    /\ byte_in_r' = 1              \* connection.input reader (server holds)
    /\ byte_out_w' = 1             \* connection.output writer (server holds)
    /\ byte_out_r' = 1             \* byte_writer holds reader
    /\ UNCHANGED <<producer, sentinel, server,
                   conn_w, conn_r, cancel_w, cancel_r,
                   guard_refs, hamt_holds_cancel, local_restored,
                   guard_destroyed_by>>

(* Server finishes echo, drops connection endpoints, exits *)

ServerExit ==
    /\ server = "running"
    /\ byte_reader \in {"running", "dead"}  \* may or may not have started
    /\ server' = "dead"
    \* Drop connections reader
    /\ conn_r' = conn_r - 1
    \* Drop connection's input reader and output writer
    /\ byte_in_r' = IF byte_in_r > 0 THEN byte_in_r - 1 ELSE 0
    /\ byte_out_w' = IF byte_out_w > 0 THEN byte_out_w - 1 ELSE 0
    /\ UNCHANGED <<producer, sentinel, byte_reader, byte_writer,
                   conn_w, cancel_w, cancel_r,
                   byte_in_w, byte_out_r,
                   guard_refs, hamt_holds_cancel, local_restored,
                   guard_destroyed_by>>

(* ByteReader sees input fd closed (client disconnected), exits *)

ByteReaderExit ==
    /\ byte_reader = "running"
    /\ byte_reader' = "dead"
    \* Drop byte_in writer
    /\ byte_in_w' = byte_in_w - 1
    /\ UNCHANGED <<producer, sentinel, server, byte_writer,
                   conn_w, conn_r, cancel_w, cancel_r,
                   byte_in_r, byte_out_w, byte_out_r,
                   guard_refs, hamt_holds_cancel, local_restored,
                   guard_destroyed_by>>

(* ByteWriter sees output channel closed (server dropped it), exits *)

ByteWriterExit ==
    /\ byte_writer = "running"
    /\ byte_writer' = "dead"
    \* Drop byte_out reader
    /\ byte_out_r' = byte_out_r - 1
    /\ UNCHANGED <<producer, sentinel, server, byte_reader,
                   conn_w, conn_r, cancel_w, cancel_r,
                   byte_in_w, byte_in_r, byte_out_w,
                   guard_refs, hamt_holds_cancel, local_restored,
                   guard_destroyed_by>>

(* Sentinel sees connections reader death (~out_copy fires), exits.
   Drops out_copy (conn writer) and its shared_ptr to cancel_guard. *)

SentinelExit ==
    /\ sentinel = "running"
    /\ conn_r = 0                   \* reader must be dead for ~out_copy to fire
    /\ sentinel' = "dead"
    \* Drop out_copy writer
    /\ conn_w' = conn_w - 1
    \* Drop shared_ptr ref to cancel_guard
    /\ guard_refs' = guard_refs - 1
    \* If last ref, guard is destroyed
    /\ IF guard_refs - 1 = 0
       THEN /\ guard_destroyed_by' = "sentinel"
            \* csp::local destructor runs HERE (sentinel's destroy_imp context)
            \* This is NOT the producer's imp context → HAMT not properly updated
            /\ local_restored' = FALSE   \* BUG: wrong context
            /\ hamt_holds_cancel' = TRUE \* HAMT still holds it (leaked)
            \* cancel_state destroyed → channels released
            /\ cancel_w' = 0
            /\ cancel_r' = 0
       ELSE /\ UNCHANGED <<cancel_w, cancel_r, hamt_holds_cancel,
                           local_restored, guard_destroyed_by>>
    /\ UNCHANGED <<producer, server, byte_reader, byte_writer,
                   conn_r, byte_in_w, byte_in_r, byte_out_w, byte_out_r>>

(* Producer catches canceled, exits.
   Drops conn writer (out) and its shared_ptr to cancel_guard. *)

ProducerExit ==
    /\ producer = "running"
    /\ producer' = "dead"
    \* Drop out writer
    /\ conn_w' = conn_w - 1
    \* Drop shared_ptr ref to cancel_guard
    /\ guard_refs' = guard_refs - 1
    \* If last ref, guard is destroyed
    /\ IF guard_refs - 1 = 0
       THEN /\ guard_destroyed_by' = "producer"
            \* csp::local destructor runs in producer's destroy_imp context
            \* This IS the correct context → HAMT properly updated
            /\ local_restored' = TRUE
            /\ hamt_holds_cancel' = FALSE
            \* cancel_state destroyed → channels released
            /\ cancel_w' = 0
            /\ cancel_r' = 0
       ELSE /\ UNCHANGED <<cancel_w, cancel_r, hamt_holds_cancel,
                           local_restored, guard_destroyed_by>>
    /\ UNCHANGED <<sentinel, server, byte_reader, byte_writer,
                   conn_r, byte_in_w, byte_in_r, byte_out_w, byte_out_r>>

-----------------------------------------------------------------------------
(* All possible transitions *)

Next ==
    \/ ServerAccept
    \/ ServerExit
    \/ ByteReaderExit
    \/ ByteWriterExit
    \/ SentinelExit
    \/ ProducerExit

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

-----------------------------------------------------------------------------
(* Type invariant *)

TypeOK ==
    /\ producer \in {"running", "dead"}
    /\ sentinel \in {"running", "dead"}
    /\ server \in {"running", "dead"}
    /\ byte_reader \in {"waiting", "running", "dead"}
    /\ byte_writer \in {"waiting", "running", "dead"}
    /\ conn_w \in 0..2
    /\ conn_r \in 0..1
    /\ cancel_w \in 0..1
    /\ cancel_r \in 0..1
    /\ byte_in_w \in 0..1
    /\ byte_in_r \in 0..1
    /\ byte_out_w \in 0..1
    /\ byte_out_r \in 0..1
    /\ guard_refs \in 0..2
    /\ hamt_holds_cancel \in BOOLEAN
    /\ local_restored \in BOOLEAN

(* Safety: after all imps are dead, no channels are held *)

AllDead ==
    /\ producer = "dead"
    /\ sentinel = "dead"
    /\ server = "dead"
    /\ byte_reader \in {"dead", "waiting"}
    /\ byte_writer \in {"dead", "waiting"}

TotalChannelRefs ==
    conn_w + conn_r + cancel_w + cancel_r +
    byte_in_w + byte_in_r + byte_out_w + byte_out_r

NoLeaks ==
    AllDead => TotalChannelRefs = 0

(* Diagnostic: the HAMT should not hold cancel_state after all imps exit *)

HamtClean ==
    AllDead => ~hamt_holds_cancel

(* Diagnostic: csp::local should be properly restored *)

LocalRestored ==
    AllDead => local_restored

=============================================================================
