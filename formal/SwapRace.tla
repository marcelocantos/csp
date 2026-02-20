---- MODULE SwapRace ----
(*******************************************************************************
 * Models the fixed swap_slots / writer_release protocol from
 * src/channel.cc after the resolve_endpoint_death re-verify loop.
 *
 * The fix: when a slot's refcount drops to 0 (last handle), the releaser
 * reads slot->channel, locks that channel's mutex, and RE-READS
 * slot->channel under the lock.  If the pointer changed (swap interleaved),
 * it unlocks and retries.  Since swap_slots holds the channel mutex while
 * writing slot->channel, the re-read under the lock is guaranteed to be
 * stable.
 *
 * This model merges the "read" and "death" steps into a single atomic
 * step (ResolveAndNotify), reflecting that the re-verify loop makes the
 * channel resolution and death notification mutually exclusive with swap.
 *
 * Participants:
 *   DoSwap   — swap_slots: atomically exchanges slot→channel and
 *              channel→write_slot_ (models the dual-locked section)
 *   EarlyRel — releases the last handle on S2
 *   LateRel  — releases the last handle on S1
 *
 * Expected result: TLC finds no invariant violations.
 *
 * Code references (src/channel.cc):
 *   ResolveAndNotify — channel.cc: Channel::resolve_endpoint_death
 *   DoSwap           — channel.cc: swap_slots (dual-locked section)
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS S1, S2,     \* Two write-side slots
          C1, C2      \* Two channels

Slots == {S1, S2}
Chans == {C1, C2}

VARIABLES
    slot_chan,     \* Slot -> Channel   (atomic: slot->channel)
    chan_wslot,    \* Channel -> Slot   (channel->write_slot_, under mu_)
    slot_ref,      \* Slot -> Nat       (atomic refcount)
    alive,         \* Channel -> Nat    (atomic alive_; 1 = write side only)
    deleted,       \* Set of deleted channels
    freed,         \* Set of freed slots
    swap_pc,       \* "ready" | "done"
    erel_pc,       \* EarlyRel: "dec" | "notify" | "done"
    erel_chan,      \* Channel resolved by EarlyRel
    lrel_pc,       \* LateRel:  "dec" | "notify" | "done"
    lrel_chan       \* Channel resolved by LateRel

vars == <<slot_chan, chan_wslot, slot_ref, alive, deleted, freed,
          swap_pc, erel_pc, erel_chan, lrel_pc, lrel_chan>>

(* ------------------------------------------------------------------
   Initial state
   ------------------------------------------------------------------ *)
Init ==
    /\ slot_chan  = [s \in Slots |-> IF s = S1 THEN C1 ELSE C2]
    /\ chan_wslot = [c \in Chans |-> IF c = C1 THEN S1 ELSE S2]
    /\ slot_ref   = [s \in Slots |-> 1]
    /\ alive      = [c \in Chans |-> 1]
    /\ deleted    = {}
    /\ freed      = {}
    /\ swap_pc    = "ready"
    /\ erel_pc    = "dec"
    /\ erel_chan  = C1      \* dummy
    /\ lrel_pc    = "dec"
    /\ lrel_chan  = C1      \* dummy

(* ------------------------------------------------------------------
   DoSwap — swap_slots dual-locked critical section (atomic)
   ------------------------------------------------------------------ *)
(* TLA:SwapRace.DoSwap
 * Code: channel.cc:557-611 (swap_slots) *)
DoSwap ==
    /\ swap_pc = "ready"
    /\ \A c \in Chans : c \notin deleted   \* swap caller holds live handles
    /\ swap_pc' = "done"
    /\ slot_chan'  = [s \in Slots |-> IF s = S1 THEN C2 ELSE C1]
    /\ chan_wslot' = [c \in Chans |-> IF c = C1 THEN S2 ELSE S1]
    /\ UNCHANGED <<slot_ref, alive, deleted, freed,
                    erel_pc, erel_chan, lrel_pc, lrel_chan>>

(* ------------------------------------------------------------------
   EarlyRel — releases the last handle on S2
   ------------------------------------------------------------------ *)

(* Step 1: fetch_sub refcount.
 * TLA:SwapRace.ERelDec
 * Code: channel.cc:535 (writer_release fetch_sub) *)
ERelDec ==
    /\ erel_pc = "dec"
    /\ slot_ref' = [slot_ref EXCEPT ![S2] = @ - 1]
    /\ erel_pc'  = "notify"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted, freed,
                    swap_pc, erel_chan, lrel_pc, lrel_chan>>

(* Step 2: resolve_endpoint_death — read channel, lock, re-verify,
 * on_endpoint_death_locked.  Atomic because the re-verify loop
 * serializes with swap under the channel mutex.
 * TLA:SwapRace.ResolveAndNotify
 * Code: channel.cc:185-203 (resolve_endpoint_death) *)
ERelNotify ==
    /\ erel_pc = "notify"
    /\ LET ch == slot_chan[S2]
       IN
          /\ erel_chan' = ch
          /\ alive' = [alive EXCEPT ![ch] = @ - 1]
          /\ IF alive[ch] = 1
             THEN /\ deleted' = deleted \cup {ch}
                  /\ freed'   = freed \cup {chan_wslot[ch]}
             ELSE /\ UNCHANGED <<deleted, freed>>
    /\ erel_pc' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref,
                    swap_pc, lrel_pc, lrel_chan>>

(* ------------------------------------------------------------------
   LateRel — releases the last handle on S1
   ------------------------------------------------------------------ *)

(* TLA:SwapRace.LRelDec
 * Code: channel.cc:549 (reader_release fetch_sub) *)
LRelDec ==
    /\ lrel_pc = "dec"
    /\ slot_ref' = [slot_ref EXCEPT ![S1] = @ - 1]
    /\ lrel_pc'  = "notify"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted, freed,
                    swap_pc, erel_pc, erel_chan, lrel_chan>>

(* TLA:SwapRace.LRelNotify
 * Code: channel.cc:185-203 (resolve_endpoint_death) *)
LRelNotify ==
    /\ lrel_pc = "notify"
    /\ LET ch == slot_chan[S1]
       IN
          /\ lrel_chan' = ch
          /\ alive' = [alive EXCEPT ![ch] = @ - 1]
          /\ IF alive[ch] = 1
             THEN /\ deleted' = deleted \cup {ch}
                  /\ freed'   = freed \cup {chan_wslot[ch]}
             ELSE /\ UNCHANGED <<deleted, freed>>
    /\ lrel_pc' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref,
                    swap_pc, erel_pc, erel_chan>>

(* ------------------------------------------------------------------
   Next-state relation
   ------------------------------------------------------------------ *)
Next ==
    \/ DoSwap
    \/ ERelDec  \/ ERelNotify
    \/ LRelDec  \/ LRelNotify

(* ------------------------------------------------------------------
   Safety invariants
   ------------------------------------------------------------------ *)

\* A releaser about to access slot memory must find it live.
NoSlotUseAfterFree ==
    /\ (erel_pc \in {"dec", "notify"} => S2 \notin freed)
    /\ (lrel_pc \in {"dec", "notify"} => S1 \notin freed)

\* A releaser about to call on_endpoint_death must target a live channel.
NoChanUseAfterFree ==
    /\ (erel_pc = "notify" => slot_chan[S2] \notin deleted)
    /\ (lrel_pc = "notify" => slot_chan[S1] \notin deleted)

\* alive counters never go negative.
AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

(* ------------------------------------------------------------------
   Spec
   ------------------------------------------------------------------ *)
Spec == Init /\ [][Next]_vars

====
