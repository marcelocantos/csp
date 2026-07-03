---- MODULE TlsConnDrain_Bug ----
(*******************************************************************************
 * BUGGY counterpart of TlsConnDrain.tla — the reverted 2026-06-14 attempt at
 * a stream-backed `tls::conn` (🎯T17.4).
 *
 * That attempt made conn::write delegate to `plaintext_out << buf` and return
 * immediately, WITHOUT waiting for the ciphertext to reach the wire.  The
 * plaintext rendezvous (rendezvous #1) completes the instant the stream imp
 * accepts the plaintext; the ciphertext is then encrypted and flushed by a
 * separate sink imp running concurrently.  So conn::write returning does not
 * imply the bytes are on the wire, and a subsequent close(fd) races the
 * sink's still-pending io::write — silently dropping the last message
 * (observed as the dropped "pong" in TLS---Handshake-and-data-roundtrip).
 *
 * The single changed action is ReturnEarly: it advances `returned` as soon as
 * the plaintext is handed over, decoupled from `flushed`.  TLC reports the
 * violation of ReturnImpliesFlushed / WireFlushedBeforeClose: the caller can
 * reach `closed` with ciphertext still unflushed.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS NumWrites

ASSUME NumWrites \in Nat

VARIABLES
    handed,    \* # conn::write calls whose plaintext the stream imp has taken
    sinkHas,   \* 0|1: serial sink holds a chunk awaiting io::write
    flushed,   \* # ciphertext chunks io::write-flushed to the wire
    returned,  \* # conn::write calls that have returned to the caller
    closed     \* caller has issued close(fd)

vars == <<handed, sinkHas, flushed, returned, closed>>

Init ==
    /\ handed   = 0
    /\ sinkHas  = 0
    /\ flushed  = 0
    /\ returned = 0
    /\ closed   = FALSE

\* conn::write hands plaintext to the stream imp.  Still serial.
StartWrite ==
    /\ handed = returned
    /\ handed < NumWrites
    /\ handed' = handed + 1
    /\ UNCHANGED <<sinkHas, flushed, returned, closed>>

\* BUG: conn::write returns as soon as the plaintext rendezvous completes,
\* with NO wait for the ciphertext flush.
ReturnEarly ==
    /\ returned < handed
    /\ returned' = returned + 1
    /\ UNCHANGED <<handed, sinkHas, flushed, closed>>

\* The ciphertext pipeline runs asynchronously, lagging behind `returned`.
EncryptPush ==
    /\ handed - flushed - sinkHas > 0
    /\ sinkHas = 0
    /\ sinkHas' = 1
    /\ UNCHANGED <<handed, flushed, returned, closed>>

Flush ==
    /\ sinkHas = 1
    /\ sinkHas' = 0
    /\ flushed' = flushed + 1
    /\ UNCHANGED <<handed, returned, closed>>

\* The caller closes the fd once every write has "returned" — but with the
\* bug, returned does not imply flushed.
Close ==
    /\ handed = NumWrites
    /\ returned = NumWrites
    /\ ~closed
    /\ closed' = TRUE
    /\ UNCHANGED <<handed, sinkHas, flushed, returned>>

Next ==
    \/ StartWrite
    \/ ReturnEarly
    \/ EncryptPush
    \/ Flush
    \/ Close

Fairness ==
    /\ WF_vars(StartWrite)
    /\ WF_vars(ReturnEarly)
    /\ WF_vars(EncryptPush)
    /\ WF_vars(Flush)
    /\ WF_vars(Close)

Spec == Init /\ [][Next]_vars /\ Fairness

TypeOK ==
    /\ handed   \in 0..NumWrites
    /\ sinkHas  \in {0, 1}
    /\ flushed  \in 0..NumWrites
    /\ returned \in 0..NumWrites
    /\ closed   \in BOOLEAN

\* Same properties as the fixed spec — these are what the bug VIOLATES.
ReturnImpliesFlushed == returned <= flushed
WireFlushedBeforeClose == closed => (flushed = NumWrites)

====
