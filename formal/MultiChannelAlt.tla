---- MODULE MultiChannelAlt ----
(*******************************************************************************
 * Models prialt_begin_impl scanning two channels, covering:
 *
 *   Phase 1: scan for ready peer or dead channel.
 *     - Data chanop on live channel: CAS claim peer → return match.
 *     - Data chanop on dead channel: defer (set dead_data_result) and
 *       continue scanning. Report dead only after all channels scanned.
 *     - Vulture (~ch) on dead channel: fire immediately, skip rest.
 *   Phase 2: no match found → register on all channels, pin alive_,
 *            set alt_state=WAITING, sleep.
 *   Phase 3: woken → lock all, deregister, unlock, unpin alive_.
 *            If alive_ reaches 0 on unpin, delete channel.
 *
 * Participants:
 *   Waiter  — imp executing prialt with a data chanop on C1 and
 *             a data chanop on C2 (both ready_flag).
 *   CloserW — thread releasing the last writer endpoint on C1.
 *   Peer    — imp registered as a waiting peer on C2 (ready to match).
 *
 * The protocol ensures:
 *   - Dead-data deferral: if C1 is dead but C2 has a ready peer, the
 *     peer match on C2 takes priority (the 1d6a732 fix).
 *   - Pin prevents delete: alive_ pin keeps channel alive during sleep
 *     even if both endpoints die.
 *   - No lost waiter: a WAITING waiter is always claimed before channel
 *     deletion.
 *
 * Code references (src/channel.cc):
 *   prialt_begin_impl — channel.cc:219-435
 *   Phase 1 scan      — channel.cc:298-349
 *   Dead-data defer   — channel.cc:306-309
 *   Vulture immediate — channel.cc:310-314
 *   Phase 2 register  — channel.cc:364-380
 *   Phase 2 sleep     — channel.cc:392-394
 *   Phase 3 deregister — channel.cc:398-406
 *   Phase 3 unpin     — channel.cc:410-417
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS C1, C2   \* Two channels

Chans == {C1, C2}

VARIABLES
    endpt_dead,     \* [Chans -> BOOLEAN]: writer endpoint dead
    alive,          \* [Chans -> Int]: alive_ counter (2 base + pins)
    channel_live,   \* [Chans -> BOOLEAN]: channel not deleted
    chan_mu,         \* [Chans -> {"none","waiter","closer"}]: mutex holder
    waiter_reg,     \* [Chans -> BOOLEAN]: waiter registered on this channel
    pinned,         \* [Chans -> BOOLEAN]: waiter has pinned this channel
    alt_state,      \* "idle" | "waiting" | "claimed"
    on_queue,       \* BOOLEAN: waiter is on run queue
    signal,         \* "none" | "match_c2" | "dead_c1"
    dead_data,      \* BOOLEAN: dead-data-result deferred from Phase 1
    peer_waiting,   \* BOOLEAN: peer is registered on C2 (WAITING)
    peer_claimed,   \* BOOLEAN: peer has been claimed by waiter
    scanned_c2,     \* BOOLEAN: ghost — waiter executed WaiterScanC2
    pc_waiter,      \* waiter program counter
    pc_closer,      \* closer program counter
    pc_peer         \* peer program counter

vars == <<endpt_dead, alive, channel_live, chan_mu, waiter_reg, pinned,
          alt_state, on_queue, signal, dead_data, peer_waiting,
          peer_claimed, scanned_c2, pc_waiter, pc_closer, pc_peer>>

(*******************************************************************************
 * Initial state.
 ******************************************************************************)
Init ==
    /\ endpt_dead   = [c \in Chans |-> FALSE]
    /\ alive        = [c \in Chans |-> 2]
    /\ channel_live = [c \in Chans |-> TRUE]
    /\ chan_mu      = [c \in Chans |-> "none"]
    /\ waiter_reg   = [c \in Chans |-> FALSE]
    /\ pinned       = [c \in Chans |-> FALSE]
    /\ alt_state    = "idle"
    /\ on_queue     = TRUE
    /\ signal       = "none"
    /\ dead_data    = FALSE
    /\ peer_waiting = FALSE
    /\ peer_claimed = FALSE
    /\ scanned_c2   = FALSE
    /\ pc_waiter    = "start"
    /\ pc_closer    = "start"
    /\ pc_peer      = "idle"

(*******************************************************************************
 * PEER ACTIONS
 *
 * Peer registers on C2 as a waiting data chanop, making itself
 * available for CAS claim by the waiter during Phase 1.
 ******************************************************************************)

(* Peer becomes available: sets peer_waiting = TRUE, modeling
 * a peer imp that has entered prialt Phase 2 on C2 and is now
 * sleeping with alt_state=WAITING.
 *
 * Code: channel.cc:364-394 (peer's own prialt Phase 2)
 *
 * TLA:MultiChannelAlt.PeerArrive *)
PeerArrive ==
    /\ pc_peer = "idle"
    /\ chan_mu[C2] = "none"  \* peer registers under C2's mutex
    /\ peer_waiting' = TRUE
    /\ pc_peer' = "waiting"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, chan_mu, waiter_reg,
                   pinned, alt_state, on_queue, signal, dead_data,
                   peer_claimed, scanned_c2, pc_waiter, pc_closer>>

(*******************************************************************************
 * WAITER ACTIONS — Phase 1
 ******************************************************************************)

(* Lock all channels in id order (C1 before C2).
 * Single action because both locks are uncontested atomically
 * from TLC's perspective (closer only locks one channel).
 *
 * Code: channel.cc:285-288 lock_all()
 *
 * TLA:MultiChannelAlt.WaiterLockAll *)
WaiterLockAll ==
    /\ pc_waiter = "start"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "waiter", ![C2] = "waiter"]
    /\ dead_data' = FALSE
    /\ scanned_c2' = FALSE
    /\ pc_waiter' = "scan_c1"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, signal, peer_waiting,
                   peer_claimed, pc_closer, pc_peer>>

(* Scan C1 (all locks held). C1 has a data chanop (ready_flag).
 * If C1's writer endpoint is dead: defer (set dead_data = TRUE).
 * If C1 is alive: no peer on C1 in this model, move on.
 *
 * Code: channel.cc:298-348 (Phase 1 scan, first channel)
 *
 * TLA:MultiChannelAlt.WaiterScanC1 *)
WaiterScanC1 ==
    /\ pc_waiter = "scan_c1"
    /\ IF endpt_dead[C1]
       THEN \* Dead channel, data chanop → defer
            /\ dead_data' = TRUE
            /\ pc_waiter' = "scan_c2"
       ELSE \* Alive, no peer on C1 → continue
            /\ pc_waiter' = "scan_c2"
            /\ UNCHANGED dead_data
    /\ UNCHANGED <<endpt_dead, alive, channel_live, chan_mu, waiter_reg,
                   pinned, alt_state, on_queue, signal, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(* Scan C2 (all locks held). C2 has a data chanop (ready_flag).
 * If C2 has a waiting peer: CAS claim → return match.
 * If C2 is dead: defer (but dead_data may already be set from C1).
 * If C2 is alive with no peer: continue to post-scan check.
 *
 * Code: channel.cc:298-348 (Phase 1 scan, second channel)
 *
 * TLA:MultiChannelAlt.WaiterScanC2 *)
WaiterScanC2 ==
    /\ pc_waiter = "scan_c2"
    /\ scanned_c2' = TRUE  \* ghost: record that we scanned C2
    /\ IF peer_waiting /\ ~peer_claimed /\ channel_live[C2]
       THEN \* Ready peer found: CAS claim
            /\ peer_claimed' = TRUE
            /\ signal' = "match_c2"
            /\ pc_waiter' = "match_found"
            /\ UNCHANGED dead_data
       ELSE IF endpt_dead[C2]
            THEN /\ dead_data' = TRUE
                 /\ pc_waiter' = "post_scan"
                 /\ UNCHANGED <<peer_claimed, signal>>
            ELSE /\ pc_waiter' = "post_scan"
                 /\ UNCHANGED <<dead_data, peer_claimed, signal>>
    /\ UNCHANGED <<endpt_dead, alive, channel_live, chan_mu, waiter_reg,
                   pinned, alt_state, on_queue, peer_waiting,
                   pc_closer, pc_peer>>

(* Match found in Phase 1: return with locks held.  alt_end will
 * unlock and schedule the peer.
 *
 * Code: channel.cc:340-343 (return with match)
 *
 * TLA:MultiChannelAlt.WaiterMatchReturn *)
WaiterMatchReturn ==
    /\ pc_waiter = "match_found"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
    /\ pc_waiter' = "done"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(* Post-scan: check dead_data. If set, report dead. Otherwise,
 * proceed to Phase 2 registration.
 *
 * Code: channel.cc:351-355 (dead_data_result check)
 *       channel.cc:357-361 (all_null / nowait check — skipped here)
 *
 * TLA:MultiChannelAlt.WaiterPostScan *)
WaiterPostScan ==
    /\ pc_waiter = "post_scan"
    /\ IF dead_data
       THEN \* Report deferred dead-data result
            /\ signal' = "dead_c1"
            /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
            /\ pc_waiter' = "done"
       ELSE \* No match, no dead — proceed to Phase 2
            /\ pc_waiter' = "register"
            /\ UNCHANGED <<signal, chan_mu>>
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(*******************************************************************************
 * WAITER ACTIONS — Phase 2 (register, pin, sleep)
 ******************************************************************************)

(* Register on all channels: set alt_state=WAITING, add to waiter lists.
 * All locks still held.
 *
 * Code: channel.cc:364-372
 *
 * TLA:MultiChannelAlt.WaiterRegister *)
WaiterRegister ==
    /\ pc_waiter = "register"
    /\ alt_state' = "waiting"
    /\ waiter_reg' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "pin"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, chan_mu, pinned,
                   on_queue, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(* Pin all channels: alive_.fetch_add(1). All locks held.
 *
 * Code: channel.cc:378-380
 *
 * TLA:MultiChannelAlt.WaiterPin *)
WaiterPin ==
    /\ pc_waiter = "pin"
    /\ alive' = [c \in Chans |-> alive[c] + 1]
    /\ pinned' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "sleep"
    /\ UNCHANGED <<endpt_dead, channel_live, chan_mu, waiter_reg,
                   alt_state, on_queue, signal, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_closer, pc_peer>>

(* Sleep: unlock all, context-switch out. Waiter leaves run queue.
 *
 * Code: channel.cc:392-394
 *
 * TLA:MultiChannelAlt.WaiterSleep *)
WaiterSleep ==
    /\ pc_waiter = "sleep"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
    /\ on_queue' = FALSE
    /\ pc_waiter' = "asleep"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(*******************************************************************************
 * WAITER ACTIONS — Phase 3 (wake, deregister, unpin)
 ******************************************************************************)

(* Wake: waiter has been claimed (on_queue=TRUE). Re-acquire all locks.
 *
 * Code: channel.cc:398
 *
 * TLA:MultiChannelAlt.WaiterWakeLock *)
WaiterWakeLock ==
    /\ pc_waiter = "asleep"
    /\ on_queue = TRUE
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "waiter", ![C2] = "waiter"]
    /\ pc_waiter' = "deregister"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(* Deregister from all channels, set alt_state=IDLE, unlock.
 *
 * Code: channel.cc:399-406, 419
 *
 * TLA:MultiChannelAlt.WaiterDeregister *)
WaiterDeregister ==
    /\ pc_waiter = "deregister"
    /\ waiter_reg' = [c \in Chans |-> FALSE]
    /\ alt_state' = "idle"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
    /\ pc_waiter' = "unpin"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, pinned,
                   on_queue, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

(* Unpin all channels: alive_.fetch_sub(1). If alive reaches 0,
 * delete channel (deferred deletion from endpoint death during sleep).
 *
 * Code: channel.cc:410-417
 *
 * TLA:MultiChannelAlt.WaiterUnpin *)
WaiterUnpin ==
    /\ pc_waiter = "unpin"
    /\ alive' = [c \in Chans |->
                    IF pinned[c] THEN alive[c] - 1 ELSE alive[c]]
    /\ channel_live' = [c \in Chans |->
                    IF pinned[c] /\ alive[c] = 1
                    THEN FALSE ELSE channel_live[c]]
    /\ pinned' = [c \in Chans |-> FALSE]
    /\ pc_waiter' = "done"
    /\ UNCHANGED <<endpt_dead, chan_mu, waiter_reg,
                   alt_state, on_queue, signal, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_closer, pc_peer>>

(*******************************************************************************
 * CLOSER ACTIONS
 *
 * CloserW kills the writer endpoint on C1, concurrent with the waiter.
 ******************************************************************************)

(* Closer marks C1's writer endpoint as dead (refcount → 0).
 *
 * Code: channel.cc:550-556
 *
 * TLA:MultiChannelAlt.CloserDecRef *)
CloserDecRef ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "want_mu"
    /\ UNCHANGED <<alive, channel_live, chan_mu, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_waiter, pc_peer>>

(* Closer acquires C1's mutex.
 *
 * Code: channel.cc:194
 *
 * TLA:MultiChannelAlt.CloserLock *)
CloserLock ==
    /\ pc_closer = "want_mu"
    /\ channel_live[C1]
    /\ chan_mu[C1] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "closer"]
    /\ pc_closer' = "wake_waiter"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_waiter, pc_peer>>

(* Closer wakes registered waiter via CAS WAITING→CLAIMED, then
 * unlocks mu_ and decrements alive_.
 *
 * Code: channel.cc:143-179 (on_endpoint_death_locked)
 *
 * TLA:MultiChannelAlt.CloserOnDeath *)
CloserOnDeath ==
    /\ pc_closer = "wake_waiter"
    /\ IF waiter_reg[C1] /\ alt_state = "waiting"
       THEN /\ alt_state' = "claimed"
            /\ on_queue' = TRUE
            /\ signal' = "dead_c1"
       ELSE UNCHANGED <<alt_state, on_queue, signal>>
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
    /\ alive' = [alive EXCEPT ![C1] = @ - 1]
    /\ channel_live' = [channel_live EXCEPT ![C1] =
                            IF alive[C1] = 1 THEN FALSE ELSE @]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<endpt_dead, waiter_reg, pinned, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_waiter, pc_peer>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ PeerArrive
    \/ WaiterLockAll
    \/ WaiterScanC1
    \/ WaiterScanC2
    \/ WaiterMatchReturn
    \/ WaiterPostScan
    \/ WaiterRegister
    \/ WaiterPin
    \/ WaiterSleep
    \/ WaiterWakeLock
    \/ WaiterDeregister
    \/ WaiterUnpin
    \/ CloserDecRef
    \/ CloserLock
    \/ CloserOnDeath

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ \A c \in Chans : endpt_dead[c] \in BOOLEAN
    /\ \A c \in Chans : alive[c] \in 0..3
    /\ \A c \in Chans : channel_live[c] \in BOOLEAN
    /\ \A c \in Chans : chan_mu[c] \in {"none", "waiter", "closer"}
    /\ \A c \in Chans : waiter_reg[c] \in BOOLEAN
    /\ \A c \in Chans : pinned[c] \in BOOLEAN
    /\ alt_state \in {"idle", "waiting", "claimed"}
    /\ on_queue \in BOOLEAN
    /\ signal \in {"none", "match_c2", "dead_c1"}
    /\ dead_data \in BOOLEAN
    /\ peer_waiting \in BOOLEAN
    /\ peer_claimed \in BOOLEAN
    /\ scanned_c2 \in BOOLEAN
    /\ pc_waiter \in {"start", "scan_c1", "scan_c2", "match_found",
                       "post_scan", "register", "pin", "sleep", "asleep",
                       "deregister", "unpin", "done"}
    /\ pc_closer \in {"start", "want_mu", "wake_waiter", "done"}
    /\ pc_peer \in {"idle", "waiting"}

(* Dead-data deferral: if the waiter reports dead and there is an
 * unclaimed peer on C2, the waiter must have scanned C2 first.
 * In the correct protocol, the waiter always scans all channels
 * before reporting dead-data, so scanned_c2 is TRUE whenever
 * signal = "dead_c1".  This is the 1d6a732 property. *)
DeadDataDeferral ==
    (pc_waiter = "done" /\ signal = "dead_c1"
        /\ peer_waiting /\ ~peer_claimed)
        => scanned_c2

(* No lost waiter: if the closer has completed and the waiter is
 * asleep with WAITING state, the waiter must be on a queue. *)
NoLostWaiter ==
    (pc_closer = "done" /\ pc_waiter = "asleep")
        => (alt_state /= "waiting" \/ on_queue)

(* Pin prevents delete: while the waiter is sleeping with a channel
 * pinned, that channel must still be live (not deleted). *)
PinPreventsDelete ==
    pc_waiter = "asleep"
        => \A c \in Chans : pinned[c] => channel_live[c]

(* No use-after-free: any action accessing channel state requires
 * channel to be live. *)
NoUseAfterFree ==
    /\ pc_waiter \in {"scan_c1", "scan_c2", "register", "pin", "sleep"}
        => \A c \in Chans : chan_mu[c] = "waiter" => channel_live[c]
    /\ pc_closer \in {"wake_waiter"}
        => channel_live[C1]

(* alive counters never go negative. *)
AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

====
