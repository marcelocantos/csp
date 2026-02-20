---- MODULE SwapRace_Bug ----
\*
\* Models the race between swap_slots() and writer_release() / on_endpoint_death()
\* in the CSP channel swap implementation.
\*
\* The bug: writer_release does two separate atomic operations:
\*   1. slot->refcount.fetch_sub(1)    -- "am I the last handle?"
\*   2. slot->channel.load()           -- "which channel owns this slot?"
\* Between (1) and (2), swap_slots can change slot->channel AND
\* channel->write_slot_. Then on_endpoint_death runs under the
\* (possibly wrong) channel's mutex and reads the post-swap write_slot_,
\* freeing the WRONG slot.
\*
\* Concrete trace TLC should find:
\*   1. ERelDec:   slot_ref[S2] = 1 -> 0
\*   2. ERelRead:  erel_chan = slot_chan[S2] = C2   (pre-swap)
\*   3. DoSwap:    slot_chan[S2] = C1, chan_wslot[C2] = S1
\*   4. ERelDeath: alive[C2] -> 0, delete chan_wslot[C2] = S1  (WRONG slot!)
\*   5. LRelDec:   touches S1 which is freed -> NoSlotUseAfterFree VIOLATED
\*
EXTENDS Integers, FiniteSets

CONSTANTS S1, S2,     \* Two write-side slots
          C1, C2      \* Two channels

Slots == {S1, S2}
Chans == {C1, C2}

VARIABLES
    slot_chan,     \* Slot -> Channel   (atomic pointer: slot->channel)
    chan_wslot,    \* Channel -> Slot   (non-atomic: channel->write_slot_, protected by mu_)
    slot_ref,      \* Slot -> Nat       (atomic refcount)
    alive,         \* Channel -> Nat    (atomic alive_ counter; 1 = write side, reader pre-dead)
    deleted,       \* Set of deleted channels
    freed,         \* Set of freed slots
    swap_pc,       \* "ready" | "done"
    erel_pc,       \* EarlyRel (releases S2): "dec" | "read" | "death" | "done"
    erel_chan,      \* Channel pointer read by EarlyRel between dec and death
    lrel_pc,       \* LateRel  (releases S1): "dec" | "read" | "death" | "done"
    lrel_chan       \* Channel pointer read by LateRel between dec and death

vars == <<slot_chan, chan_wslot, slot_ref, alive, deleted, freed,
          swap_pc, erel_pc, erel_chan, lrel_pc, lrel_chan>>

\* ------------------------------------------------------------------
\* Initial state
\* ------------------------------------------------------------------
\* Two channels, each with its own write slot. Refcount 1 = one writer
\* handle remaining per slot. alive = 1 = only write side alive (reader
\* side already dead, simplifying the model).
Init ==
    /\ slot_chan  = [s \in Slots |-> IF s = S1 THEN C1 ELSE C2]
    /\ chan_wslot = [c \in Chans |-> IF c = C1 THEN S1 ELSE S2]
    /\ slot_ref   = [s \in Slots |-> 1]
    /\ alive      = [c \in Chans |-> 1]
    /\ deleted    = {}
    /\ freed      = {}
    /\ swap_pc    = "ready"
    /\ erel_pc    = "dec"
    /\ erel_chan  = C1      \* dummy, overwritten by ERelRead
    /\ lrel_pc    = "dec"
    /\ lrel_chan  = C1      \* dummy, overwritten by LRelRead

\* ------------------------------------------------------------------
\* swap_slots: atomic (models the dual-locked critical section)
\* ------------------------------------------------------------------
\* Swaps slot->channel pointers and channel->write_slot_ back-pointers.
\* In the real code this holds both channel mutexes.
DoSwap ==
    /\ swap_pc = "ready"
    /\ swap_pc' = "done"
    /\ slot_chan'  = [s \in Slots |-> IF s = S1 THEN C2 ELSE C1]
    /\ chan_wslot' = [c \in Chans |-> IF c = C1 THEN S2 ELSE S1]
    /\ UNCHANGED <<slot_ref, alive, deleted, freed,
                    erel_pc, erel_chan, lrel_pc, lrel_chan>>

\* ------------------------------------------------------------------
\* EarlyRel: releases the last handle on S2
\* ------------------------------------------------------------------
\* Models writer_release(S2):
\*   dec:   slot->refcount.fetch_sub(1)  -- separate atomic
\*   read:  slot->channel.load()         -- separate atomic (THE GAP)
\*   death: on_endpoint_death(ch) under ch->mu_

ERelDec ==
    /\ erel_pc = "dec"
    /\ slot_ref' = [slot_ref EXCEPT ![S2] = @ - 1]
    /\ erel_pc'  = "read"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted, freed,
                    swap_pc, erel_chan, lrel_pc, lrel_chan>>

ERelRead ==
    /\ erel_pc = "read"
    /\ erel_chan' = slot_chan[S2]    \* may read pre- or post-swap value
    /\ erel_pc'  = "death"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref, alive, deleted, freed,
                    swap_pc, lrel_pc, lrel_chan>>

\* on_endpoint_death runs under erel_chan's mutex.
\* Reads chan_wslot[erel_chan] (post-swap if swap already ran and released
\* the mutex). If alive reaches 0, deletes channel + its current write_slot_.
ERelDeath ==
    /\ erel_pc = "death"
    /\ alive' = [alive EXCEPT ![erel_chan] = @ - 1]
    /\ IF alive[erel_chan] = 1
       THEN \* Last alive: delete channel and its write_slot_
            /\ deleted' = deleted \cup {erel_chan}
            /\ freed'   = freed \cup {chan_wslot[erel_chan]}
       ELSE
            /\ UNCHANGED <<deleted, freed>>
    /\ erel_pc' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref,
                    swap_pc, erel_chan, lrel_pc, lrel_chan>>

\* ------------------------------------------------------------------
\* LateRel: releases the last handle on S1
\* ------------------------------------------------------------------
\* Same three-step protocol as EarlyRel but for slot S1.

LRelDec ==
    /\ lrel_pc = "dec"
    /\ slot_ref' = [slot_ref EXCEPT ![S1] = @ - 1]
    /\ lrel_pc'  = "read"
    /\ UNCHANGED <<slot_chan, chan_wslot, alive, deleted, freed,
                    swap_pc, erel_pc, erel_chan, lrel_chan>>

LRelRead ==
    /\ lrel_pc = "read"
    /\ lrel_chan' = slot_chan[S1]
    /\ lrel_pc'  = "death"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref, alive, deleted, freed,
                    swap_pc, erel_pc, erel_chan>>

LRelDeath ==
    /\ lrel_pc = "death"
    /\ alive' = [alive EXCEPT ![lrel_chan] = @ - 1]
    /\ IF alive[lrel_chan] = 1
       THEN
            /\ deleted' = deleted \cup {lrel_chan}
            /\ freed'   = freed \cup {chan_wslot[lrel_chan]}
       ELSE
            /\ UNCHANGED <<deleted, freed>>
    /\ lrel_pc' = "done"
    /\ UNCHANGED <<slot_chan, chan_wslot, slot_ref,
                    swap_pc, erel_pc, erel_chan, lrel_chan>>

\* ------------------------------------------------------------------
\* Next-state relation
\* ------------------------------------------------------------------
Next ==
    \/ DoSwap
    \/ ERelDec  \/ ERelRead  \/ ERelDeath
    \/ LRelDec  \/ LRelRead  \/ LRelDeath

\* ------------------------------------------------------------------
\* Safety invariants
\* ------------------------------------------------------------------

\* A releaser about to access slot memory must find it live.
NoSlotUseAfterFree ==
    /\ (erel_pc \in {"dec", "read"} => S2 \notin freed)
    /\ (lrel_pc \in {"dec", "read"} => S1 \notin freed)

\* A releaser about to call on_endpoint_death must target a live channel.
NoChanUseAfterFree ==
    /\ (erel_pc = "death" => erel_chan \notin deleted)
    /\ (lrel_pc = "death" => lrel_chan \notin deleted)

\* alive counters never go negative.
AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

\* ------------------------------------------------------------------
\* Spec
\* ------------------------------------------------------------------
Spec == Init /\ [][Next]_vars

====
