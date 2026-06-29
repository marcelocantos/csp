---- MODULE TlsConnDrain ----
(*******************************************************************************
 * Models the wire-synchronous write barrier in the stream-backed `tls::conn`
 * (🎯T17.4, design paper docs/papers/29-tls-conn-over-stream.md, option (c)).
 *
 * `conn::write` must preserve the original direct-picotls contract: when it
 * returns, the ciphertext is already on the wire, so a subsequent caller-side
 * `close(fd)` cannot race in-flight bytes.  In the stream-backed design the
 * ciphertext crosses an imp boundary:
 *
 *     conn::write:   plaintext_out << buf          (rendezvous with stream imp)
 *     stream imp:    ptls_send -> up_out << cipher  (one push per write, k=1)
 *     conn sink:     cipher_r >> chunk -> io::write -> wire_ack << {}
 *     conn::write:   wire_ack_r >> {}               (BLOCKS for the flush ack)
 *
 * The load-bearing facts the design relies on:
 *
 *   - conn is SERIAL: it never issues the next plaintext write until the
 *     current conn::write has returned (its byte interface is synchronous).
 *   - The stream->sink channel is UNBUFFERED and the sink is SERIAL, so a
 *     chunk is taken by the sink only when the sink is free to write it.
 *   - One steady-state plaintext write produces exactly one ciphertext push
 *     (send_plain does one ptls_send -> one up_out<<), so conn::write blocks
 *     for exactly one wire ack.
 *
 * SAFETY:  a conn::write returns only after its ciphertext has been
 *          io::write-flushed (returned <= flushed), hence close(fd) — issued
 *          only after every write returns — never races un-flushed ciphertext
 *          (closed => flushed = NumWrites).
 *
 * The buggy counterpart TlsConnDrain_Bug.tla drops the ack wait (the
 * reverted 2026-06-14 behaviour) and TLC reports the close-vs-flush race.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS NumWrites          \* number of conn::write calls the caller makes

ASSUME NumWrites \in Nat

VARIABLES
    handed,    \* # conn::write calls whose plaintext the stream imp has taken
    sinkHas,   \* 0|1: serial conn-owned sink holds a chunk awaiting io::write
    flushed,   \* # ciphertext chunks the sink has io::write-flushed to the wire
    ackAvail,  \* 0|1: wire ack posted by the sink, not yet consumed (depth-1)
    returned,  \* # conn::write calls that have returned to the caller
    closed     \* caller has issued close(fd) (only after all writes returned)

vars == <<handed, sinkHas, flushed, ackAvail, returned, closed>>

Init ==
    /\ handed   = 0
    /\ sinkHas  = 0
    /\ flushed  = 0
    /\ ackAvail = 0
    /\ returned = 0
    /\ closed   = FALSE

\* conn::write begins: `plaintext_out << buf` hands plaintext to the stream
\* imp.  conn is serial — never starts a write while one is outstanding.
StartWrite ==
    /\ handed = returned                  \* conn-serial invariant: nothing in flight
    /\ handed < NumWrites
    /\ handed' = handed + 1
    /\ UNCHANGED <<sinkHas, flushed, ackAvail, returned, closed>>

\* stream imp: ptls_send encrypts the handed plaintext into one chunk and
\* pushes it onto the UNBUFFERED stream->sink channel; the serial sink takes
\* it in the same rendezvous (sinkHas 0 -> 1).  One push per write (k=1).
EncryptPush ==
    /\ handed - flushed - sinkHas > 0     \* a handed plaintext not yet encrypted
    /\ sinkHas = 0                         \* serial sink is free to take
    /\ sinkHas' = 1
    /\ UNCHANGED <<handed, flushed, ackAvail, returned, closed>>

\* sink: io::write the held chunk to completion, then post the wire ack.
\* Depth-1 ack channel: a new ack is posted only once the prior was consumed.
Flush ==
    /\ sinkHas = 1
    /\ ackAvail = 0
    /\ sinkHas' = 0
    /\ flushed' = flushed + 1
    /\ ackAvail' = 1
    /\ UNCHANGED <<handed, returned, closed>>

\* conn::write returns: it BLOCKS for the wire ack, so it returns only after
\* the sink has io::write-flushed this write's ciphertext.
ConsumeAck ==
    /\ ackAvail = 1
    /\ returned < handed
    /\ ackAvail' = 0
    /\ returned' = returned + 1
    /\ UNCHANGED <<handed, sinkHas, flushed, closed>>

\* The caller issues close(fd) once every write has returned.
Close ==
    /\ handed = NumWrites
    /\ returned = NumWrites
    /\ ~closed
    /\ closed' = TRUE
    /\ UNCHANGED <<handed, sinkHas, flushed, ackAvail, returned>>

Next ==
    \/ StartWrite
    \/ EncryptPush
    \/ Flush
    \/ ConsumeAck
    \/ Close

Fairness ==
    /\ WF_vars(StartWrite)
    /\ WF_vars(EncryptPush)
    /\ WF_vars(Flush)
    /\ WF_vars(ConsumeAck)
    /\ WF_vars(Close)

Spec == Init /\ [][Next]_vars /\ Fairness

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ handed   \in 0..NumWrites
    /\ sinkHas  \in {0, 1}
    /\ flushed  \in 0..NumWrites
    /\ ackAvail \in {0, 1}
    /\ returned \in 0..NumWrites
    /\ closed   \in BOOLEAN

\* CORE SAFETY: a conn::write returns only after its ciphertext is flushed.
ReturnImpliesFlushed == returned <= flushed

\* WIRE-SYNC AT CLOSE: when the caller closes the fd, every chunk of every
\* write is already on the wire — no ciphertext can race the close.
WireFlushedBeforeClose == closed => (flushed = NumWrites)

\* LIVENESS: the connection always finishes (every write returns, fd closes).
Terminates == <>closed

====
