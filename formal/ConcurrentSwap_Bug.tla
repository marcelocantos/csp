---- MODULE ConcurrentSwap_Bug ----
(*******************************************************************************
 * Bug variant of ConcurrentSwap: removes the re-verify step from both
 * swappers.  Without re-verification, a swap can exchange with stale
 * channel pointers after a concurrent swap has already modified the
 * slot→channel mapping.
 *
 * Expected TLC result: ConsistentFinalState violation.
 *
 * Concrete trace:
 *   1. S1Read: s1_ca=C1, s1_cb=C2 (SA→C1, SB→C2)
 *   2. S2Read: s2_ca=C2, s2_cb=C3 (SB→C2, SC→C3)
 *   3. S2LockA, S2LockB: Swap2 locks C2, C3
 *   4. S2Exchange: SB→C3, SC→C2.  chan_wslot: C2→SC, C3→SB
 *   5. S2Unlock
 *   6. S1LockA, S1LockB: Swap1 locks C1, C2 (but SB is now on C3!)
 *   7. S1Exchange (NO VERIFY): uses stale s1_cb=C2.
 *      Sets SA→C2, SB→C1. chan_wslot: C1→SB, C2→SA.
 *      But SB.channel was C3 (set by Swap2), now overwritten to C1.
 *      C3.write_slot_ still points to SB (from Swap2), but SB→C1.
 *      chan_wslot[C3]=SB, slot_chan[SB]=C1 → INCONSISTENT.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    SA, SB, SC,
    C1, C2, C3

Slots == {SA, SB, SC}
Chans == {C1, C2, C3}

ChanID(c) == CASE c = C1 -> 1 [] c = C2 -> 2 [] c = C3 -> 3

VARIABLES
    slot_chan, chan_wslot, chan_mu, alive, deleted,
    s1_sa, s1_sb, s1_ca, s1_cb, pc_s1,
    s2_sa, s2_sb, s2_ca, s2_cb, pc_s2,
    pc_closer

vars == <<slot_chan, chan_wslot, chan_mu, alive, deleted,
          s1_sa, s1_sb, s1_ca, s1_cb, pc_s1,
          s2_sa, s2_sb, s2_ca, s2_cb, pc_s2,
          pc_closer>>

Init ==
    /\ slot_chan  = [s \in Slots |-> CASE s = SA -> C1 [] s = SB -> C2 [] s = SC -> C3]
    /\ chan_wslot = [c \in Chans |-> CASE c = C1 -> SA [] c = C2 -> SB [] c = C3 -> SC]
    /\ chan_mu    = [c \in Chans |-> "none"]
    /\ alive     = [c \in Chans |-> 2]
    /\ deleted   = {}
    /\ s1_sa = SA /\ s1_sb = SB
    /\ s1_ca = C1 /\ s1_cb = C1
    /\ pc_s1 = "read"
    /\ s2_sa = SB /\ s2_sb = SC
    /\ s2_ca = C1 /\ s2_cb = C1
    /\ pc_s2 = "read"
    /\ pc_closer = "start"

\* ---- SWAP1 ----

S1Read ==
    /\ pc_s1 = "read"
    /\ LET ca == slot_chan[SA]
           cb == slot_chan[SB]
       IN IF ca = cb
          THEN /\ pc_s1' = "done"
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

S1LockA ==
    /\ pc_s1 = "lock_a"
    /\ s1_ca \notin deleted
    /\ chan_mu[s1_ca] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_ca] = "s1"]
    /\ pc_s1' = "lock_b"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

S1LockB ==
    /\ pc_s1 = "lock_b"
    /\ s1_cb \notin deleted
    /\ chan_mu[s1_cb] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_cb] = "s1"]
    \* BUG: skip verify, go straight to exchange
    /\ pc_s1' = "exchange"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

S1Exchange ==
    /\ pc_s1 = "exchange"
    /\ slot_chan' = [slot_chan EXCEPT ![s1_sa] = s1_cb, ![s1_sb] = s1_ca]
    /\ chan_wslot' = [chan_wslot EXCEPT ![s1_ca] = s1_sb, ![s1_cb] = s1_sa]
    /\ pc_s1' = "unlock"
    /\ UNCHANGED <<chan_mu, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

S1Unlock ==
    /\ pc_s1 = "unlock"
    /\ chan_mu' = [chan_mu EXCEPT ![s1_ca] = "none", ![s1_cb] = "none"]
    /\ pc_s1' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s1_sa, s1_sb, s1_ca, s1_cb,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2, pc_closer>>

\* ---- SWAP2 ----

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

S2LockA ==
    /\ pc_s2 = "lock_a"
    /\ s2_ca \notin deleted
    /\ chan_mu[s2_ca] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_ca] = "s2"]
    /\ pc_s2' = "lock_b"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

S2LockB ==
    /\ pc_s2 = "lock_b"
    /\ s2_cb \notin deleted
    /\ chan_mu[s2_cb] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_cb] = "s2"]
    \* BUG: skip verify, go straight to exchange
    /\ pc_s2' = "exchange"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

S2Exchange ==
    /\ pc_s2 = "exchange"
    /\ slot_chan' = [slot_chan EXCEPT ![s2_sa] = s2_cb, ![s2_sb] = s2_ca]
    /\ chan_wslot' = [chan_wslot EXCEPT ![s2_ca] = s2_sb, ![s2_cb] = s2_sa]
    /\ pc_s2' = "unlock"
    /\ UNCHANGED <<chan_mu, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

S2Unlock ==
    /\ pc_s2 = "unlock"
    /\ chan_mu' = [chan_mu EXCEPT ![s2_ca] = "none", ![s2_cb] = "none"]
    /\ pc_s2' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted,
                   s2_sa, s2_sb, s2_ca, s2_cb,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1, pc_closer>>

\* ---- CLOSER ----

CloserDeath ==
    /\ pc_closer = "start"
    /\ LET ch == slot_chan[SC]
       IN /\ ch \notin deleted
          /\ chan_mu[ch] = "none"
          /\ alive' = [alive EXCEPT ![ch] = @ - 1]
          /\ IF alive[ch] = 1
             THEN deleted' = deleted \cup {ch}
             ELSE UNCHANGED deleted
    /\ pc_closer' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, chan_mu,
                   s1_sa, s1_sb, s1_ca, s1_cb, pc_s1,
                   s2_sa, s2_sb, s2_ca, s2_cb, pc_s2>>

\* ---- SPEC ----

Next ==
    \/ S1Read \/ S1LockA \/ S1LockB \/ S1Exchange \/ S1Unlock
    \/ S2Read \/ S2LockA \/ S2LockB \/ S2Exchange \/ S2Unlock
    \/ CloserDeath

Spec == Init /\ [][Next]_vars

\* ---- PROPERTIES (same as correct spec) ----

TypeOK ==
    /\ \A s \in Slots : slot_chan[s] \in Chans
    /\ \A c \in Chans : chan_wslot[c] \in Slots
    /\ \A c \in Chans : chan_mu[c] \in {"none", "s1", "s2", "closer"}
    /\ \A c \in Chans : alive[c] \in 0..2
    /\ deleted \subseteq Chans
    /\ pc_s1 \in {"read", "lock_a", "lock_b", "exchange", "unlock", "done"}
    /\ pc_s2 \in {"read", "lock_a", "lock_b", "exchange", "unlock", "done"}
    /\ pc_closer \in {"start", "done"}

ConsistentFinalState ==
    (pc_s1 = "done" /\ pc_s2 = "done") =>
        \A s \in Slots :
            LET ch == slot_chan[s]
            IN ch \notin deleted => chan_wslot[ch] = s

AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

====
