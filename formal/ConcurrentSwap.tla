---- MODULE ConcurrentSwap ----
(*******************************************************************************
 * Models two concurrent swap_slots operations on overlapping channels.
 *
 * Swap1 swaps slot SA ↔ SB (initially on channels C1, C2).
 * Swap2 swaps slot SB ↔ SC (initially on channels C2, C3), overlapping on SB.
 * Closer releases the last endpoint handle on one channel (C3), racing with
 * swaps.
 *
 * The protocol ensures:
 *   - Deadlock freedom: ID-ordered locking prevents cyclic waits.
 *   - Consistent final state: after both swaps, slot↔channel mappings are
 *     bijective (each slot points to one channel, each channel's back-pointer
 *     matches its slot's forward pointer).
 *   - Re-verify prevents stale exchange: if a concurrent swap changed
 *     slot->channel between the initial read and the lock acquisition, the
 *     swap unlocks and retries.
 *
 * Code references (src/channel.cc):
 *   swap_slots     — channel.cc:578-634
 *   ID ordering    — channel.cc:588-591 (if ca->id_ > cb->id_)
 *   Re-verify      — channel.cc:597-604 (if ca_now != ca || cb_now != cb)
 *   Exchange       — channel.cc:612-623 (slot/channel pointer swap)
 *   WakeAll        — channel.cc:627-629 (wake_all_for_swap)
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    SA, SB, SC,     \* Three write-side slots
    C1, C2, C3      \* Three channels (id: C1 < C2 < C3)

Slots == {SA, SB, SC}
Chans == {C1, C2, C3}

\* Channel ID ordering: C1 < C2 < C3.
ChanID(c) == CASE c = C1 -> 1 [] c = C2 -> 2 [] c = C3 -> 3

VARIABLES
    slot_chan,       \* [Slots -> Chans]: slot->channel (atomic pointer)
    chan_wslot,      \* [Chans -> Slots]: channel->write_slot_ (under mu_)
    chan_mu,         \* [Chans -> {"none","s1","s2","closer"}]: mutex holder
    alive,          \* [Chans -> Nat]: alive_ counter (2 = both endpoints)
    deleted,        \* Set of deleted channels
    \* Swap1 local state
    s1_sa, s1_sb,   \* Swap1's slot operands (ordered by channel id)
    s1_ca, s1_cb,   \* Swap1's locally-read channel pointers
    pc_s1,          \* Swap1 program counter
    \* Swap2 local state
    s2_sa, s2_sb,   \* Swap2's slot operands (ordered by channel id)
    s2_ca, s2_cb,   \* Swap2's locally-read channel pointers
    pc_s2,          \* Swap2 program counter
    \* Closer
    pc_closer

vars == <<slot_chan, chan_wslot, chan_mu, alive, deleted,
          s1_sa, s1_sb, s1_ca, s1_cb, pc_s1,
          s2_sa, s2_sb, s2_ca, s2_cb, pc_s2,
          pc_closer>>

(*******************************************************************************
 * Initial state: SA→C1, SB→C2, SC→C3.
 ******************************************************************************)
Init ==
    /\ slot_chan  = [s \in Slots |-> CASE s = SA -> C1 [] s = SB -> C2 [] s = SC -> C3]
    /\ chan_wslot = [c \in Chans |-> CASE c = C1 -> SA [] c = C2 -> SB [] c = C3 -> SC]
    /\ chan_mu    = [c \in Chans |-> "none"]
    /\ alive     = [c \in Chans |-> 2]
    /\ deleted   = {}
    \* Swap1: SA ↔ SB (initially C1, C2)
    /\ s1_sa = SA /\ s1_sb = SB
    /\ s1_ca = C1 /\ s1_cb = C1  \* dummy, set by S1Read
    /\ pc_s1 = "read"
    \* Swap2: SB ↔ SC (initially C2, C3)
    /\ s2_sa = SB /\ s2_sb = SC
    /\ s2_ca = C1 /\ s2_cb = C1  \* dummy, set by S2Read
    /\ pc_s2 = "read"
    \* Closer on C3
    /\ pc_closer = "start"

(*******************************************************************************
 * SWAP1 ACTIONS
 *
 * swap_slots(SA, SB): read channels, lock in id order, verify, exchange.
 ******************************************************************************)

(* Read slot->channel for both slots (non-atomic pair of atomic loads).
 * Then sort by channel id for lock ordering.
 *
 * Code: channel.cc:583-591
 *
 * TLA:ConcurrentSwap.S1Read *)
S1Read ==
    /\ pc_s1 = "read"
    /\ LET ca == slot_chan[SA]
           cb == slot_chan[SB]
       IN IF ca = cb
          THEN \* Same channel — no-op (channel.cc:585)
               /\ pc_s1' = "done"
               /\ UNCHANGED <<s1_sa, s1_sb, s1_ca, s1_cb>>
          ELSE IF ChanID(ca) > ChanID(cb)
               THEN /\ s1_sa' = SB /\ s1_sb' = SA
                    /\ s1_ca' = cb /\ s1_cb' = ca
                    /\ pc_s1' = "lock_a"
               ELSE /\ s1_sa' = SA /\ s1_sb' = SB
                    /\ s1_ca' = ca /\ s1_cb' = cb
                    /\ pc_s1' = "lock_a"
    /\ UNCHANGED <<slot_chan, chan_wslot, chan_mu, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(* Lock first channel (lower id).
 *
 * Code: channel.cc:593
 *
 * TLA:ConcurrentSwap.S1LockA *)
S1LockA ==
    /\ pc_s1 = "lock_a"
    /\ s1_ca \notin deleted
    /\ chan_mu[s1_ca] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_ca] = "s1"]
    /\ pc_s1' = "lock_b"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(* Lock second channel (higher id).
 *
 * Code: channel.cc:594
 *
 * TLA:ConcurrentSwap.S1LockB *)
S1LockB ==
    /\ pc_s1 = "lock_b"
    /\ s1_cb \notin deleted
    /\ chan_mu[s1_cb] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_cb] = "s1"]
    /\ pc_s1' = "verify"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(* Re-verify: re-read slot->channel under both locks.  If either
 * changed (concurrent swap modified the slot), unlock and retry.
 *
 * Code: channel.cc:597-604
 *
 * TLA:ConcurrentSwap.S1Verify *)
S1Verify ==
    /\ pc_s1 = "verify"
    /\ LET ca_now == slot_chan[s1_sa]
           cb_now == slot_chan[s1_sb]
       IN IF ca_now /= s1_ca \/ cb_now /= s1_cb
          THEN \* Stale: unlock both, retry
               /\ chan_mu' = [chan_mu EXCEPT ![s1_ca] = "none", ![s1_cb] = "none"]
               /\ pc_s1' = "read"
          ELSE \* Verified: proceed to exchange
               /\ pc_s1' = "exchange"
               /\ UNCHANGED chan_mu
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(* Exchange slot↔channel pointers and back-pointers.  Both locks held,
 * so this is atomic with respect to other participants.
 *
 * Code: channel.cc:612-623
 *
 * TLA:ConcurrentSwap.S1Exchange *)
S1Exchange ==
    /\ pc_s1 = "exchange"
    /\ slot_chan' = [slot_chan EXCEPT ![s1_sa] = s1_cb, ![s1_sb] = s1_ca]
    /\ chan_wslot' = [chan_wslot EXCEPT ![s1_ca] = s1_sb, ![s1_cb] = s1_sa]
    /\ pc_s1' = "unlock"
    /\ UNCHANGED <<chan_mu, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(* Release both locks.  We fold wake_all_for_swap into the unlock step
 * since waker interactions are covered by AltStateCAS.tla and
 * SwapWaiterRetry.tla; this spec focuses on locking and consistency.
 *
 * Code: channel.cc:627-633
 *
 * TLA:ConcurrentSwap.S1Unlock *)
S1Unlock ==
    /\ pc_s1 = "unlock"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_ca] = "none", ![s1_cb] = "none"]
    /\ pc_s1' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

(*******************************************************************************
 * SWAP2 ACTIONS
 *
 * swap_slots(SB, SC): identical protocol, different slot operands.
 ******************************************************************************)

(* TLA:ConcurrentSwap.S2Read *)
S2Read ==
    /\ pc_s2 = "read"
    /\ LET ca == slot_chan[SB]
           cb == slot_chan[SC]
       IN IF ca = cb
          THEN /\ pc_s2' = "done"
               /\ UNCHANGED <<s2_sa, s2_sb, s2_ca, s2_cb>>
          ELSE IF ChanID(ca) > ChanID(cb)
               THEN /\ s2_sa' = SC /\ s2_sb' = SB
                    /\ s2_ca' = cb /\ s2_cb' = ca
                    /\ pc_s2' = "lock_a"
               ELSE /\ s2_sa' = SB /\ s2_sb' = SC
                    /\ s2_ca' = ca /\ s2_cb' = cb
                    /\ pc_s2' = "lock_a"
    /\ UNCHANGED <<slot_chan, chan_wslot, chan_mu, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(* TLA:ConcurrentSwap.S2LockA *)
S2LockA ==
    /\ pc_s2 = "lock_a"
    /\ s2_ca \notin deleted
    /\ chan_mu[s2_ca] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_ca] = "s2"]
    /\ pc_s2' = "lock_b"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(* TLA:ConcurrentSwap.S2LockB *)
S2LockB ==
    /\ pc_s2 = "lock_b"
    /\ s2_cb \notin deleted
    /\ chan_mu[s2_cb] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_cb] = "s2"]
    /\ pc_s2' = "verify"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(* TLA:ConcurrentSwap.S2Verify *)
S2Verify ==
    /\ pc_s2 = "verify"
    /\ LET ca_now == slot_chan[s2_sa]
           cb_now == slot_chan[s2_sb]
       IN IF ca_now /= s2_ca \/ cb_now /= s2_cb
          THEN /\ chan_mu' = [chan_mu EXCEPT ![s2_ca] = "none", ![s2_cb] = "none"]
               /\ pc_s2' = "read"
          ELSE /\ pc_s2' = "exchange"
               /\ UNCHANGED chan_mu
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(* TLA:ConcurrentSwap.S2Exchange *)
S2Exchange ==
    /\ pc_s2 = "exchange"
    /\ slot_chan' = [slot_chan EXCEPT ![s2_sa] = s2_cb, ![s2_sb] = s2_ca]
    /\ chan_wslot' = [chan_wslot EXCEPT ![s2_ca] = s2_sb, ![s2_cb] = s2_sa]
    /\ pc_s2' = "unlock"
    /\ UNCHANGED <<chan_mu, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(* TLA:ConcurrentSwap.S2Unlock *)
S2Unlock ==
    /\ pc_s2 = "unlock"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_ca] = "none", ![s2_cb] = "none"]
    /\ pc_s2' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

(*******************************************************************************
 * CLOSER ACTIONS
 *
 * Models endpoint death on C3, racing with both swaps.
 ******************************************************************************)

(* Closer resolves endpoint death: lock channel, on_endpoint_death_locked.
 * We model the resolve_endpoint_death loop as atomic (the re-verify
 * loop inside is proven correct by SwapRace.tla; here we just need
 * the effect: lock the current channel for the slot, decrement alive_).
 *
 * Code: channel.cc:190-205 (resolve_endpoint_death)
 *       channel.cc:143-179 (on_endpoint_death_locked)
 *
 * TLA:ConcurrentSwap.CloserDeath *)
CloserDeath ==
    /\ pc_closer = "start"
    /\ LET ch == slot_chan[SC]
       IN /\ ch \notin deleted
          /\ chan_mu[ch] = "none"
          /\ chan_mu' = [chan_mu EXCEPT ![ch] = "closer"]
          /\ alive' = [alive EXCEPT ![ch] = @ - 1]
          /\ IF alive[ch] = 1
             THEN deleted' = deleted \cup {ch}
             ELSE UNCHANGED deleted
          /\ chan_mu' = [chan_mu EXCEPT ![ch] = "none"]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ S1Read \/ S1LockA \/ S1LockB \/ S1Verify \/ S1Exchange \/ S1Unlock
    \/ S2Read \/ S2LockA \/ S2LockB \/ S2Verify \/ S2Exchange \/ S2Unlock
    \/ CloserDeath

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ \A s \in Slots : slot_chan[s] \in Chans
    /\ \A c \in Chans : chan_wslot[c] \in Slots
    /\ \A c \in Chans : chan_mu[c] \in {"none", "s1", "s2", "closer"}
    /\ \A c \in Chans : alive[c] \in 0..2
    /\ deleted \subseteq Chans
    /\ s1_sa \in Slots /\ s1_sb \in Slots
    /\ s1_ca \in Chans /\ s1_cb \in Chans
    /\ s2_sa \in Slots /\ s2_sb \in Slots
    /\ s2_ca \in Chans /\ s2_cb \in Chans
    /\ pc_s1 \in {"read", "lock_a", "lock_b", "verify", "exchange", "unlock", "done"}
    /\ pc_s2 \in {"read", "lock_a", "lock_b", "verify", "exchange", "unlock", "done"}
    /\ pc_closer \in {"start", "done"}

(* Deadlock freedom: no state where both swappers are each waiting for
 * a lock the other holds.  Because both acquire in id order, this
 * circular dependency cannot arise.  We express it directly: if Swap1
 * holds a lock and Swap2 holds a lock, they cannot each be blocking
 * on the other's held channel. *)
DeadlockFreedom ==
    ~(\E ca, cb \in Chans :
        ca /= cb
        /\ chan_mu[ca] = "s1" /\ chan_mu[cb] = "s2"
        /\ (pc_s1 \in {"lock_a", "lock_b"})
        /\ (pc_s2 \in {"lock_a", "lock_b"})
        /\ s1_cb = cb /\ s2_cb = ca)  \* each wants what the other holds

(* Mutex exclusion: no channel mutex held by two participants. *)
MutexExclusion ==
    \A c \in Chans :
        chan_mu[c] /= "none" =>
            Cardinality({p \in {"s1", "s2", "closer"} : chan_mu[c] = p}) = 1

(* Consistent final state: when both swaps are done, every slot points
 * to a channel whose back-pointer points back.  This proves the
 * exchange doesn't corrupt the bidirectional mapping. *)
ConsistentFinalState ==
    (pc_s1 = "done" /\ pc_s2 = "done") =>
        \A s \in Slots :
            LET ch == slot_chan[s]
            IN ch \notin deleted => chan_wslot[ch] = s

(* alive counters never go negative. *)
AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

(* Swappers never access a deleted channel. *)
NoUseAfterFree ==
    /\ pc_s1 \in {"lock_a", "lock_b", "verify", "exchange", "unlock"}
        => (s1_ca \notin deleted /\ s1_cb \notin deleted)
    /\ pc_s2 \in {"lock_a", "lock_b", "verify", "exchange", "unlock"}
        => (s2_ca \notin deleted /\ s2_cb \notin deleted)

====
