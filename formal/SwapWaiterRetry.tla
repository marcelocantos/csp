---- MODULE SwapWaiterRetry ----
(*******************************************************************************
 * Models swap_slots firing while a waiter is sleeping in prialt.
 *
 * The waiter resolves slot→channel mappings, registers on channels,
 * and sleeps.  swap_slots atomically exchanges slot→channel pointers
 * and wakes all registered waiters with signal=INT_MIN.  On wake,
 * the waiter must re-resolve slot→channel through the Slot indirection
 * before re-registering, so that chanops target the correct channels.
 *
 * Participants:
 *   Waiter  — imp executing prialt with chanops on two slots (S1,S2).
 *   Swapper — thread executing swap_slots(S1, S2).
 *   Closer  — thread releasing the last writer endpoint on C1.
 *
 * The protocol ensures:
 *   - Re-resolution after swap: waiter re-reads slot→channel before
 *     re-entering Phase 1, so chanops always target current channels.
 *   - Pin prevents delete: alive_ pin keeps channels alive during sleep
 *     even when concurrent endpoint death fires.
 *   - CAS exclusion: only one waker (swap or closer) claims the waiter.
 *
 * Code references (src/channel.cc):
 *   swap_slots            — channel.cc:578-634
 *   ID-ordered locking    — channel.cc:588-591
 *   Exchange              — channel.cc:612-623
 *   wake_all_for_swap     — channel.cc:120-136
 *   prialt_begin_impl     — channel.cc:219-435
 *   Slot re-resolution    — channel.cc:226-239
 *   Phase 2 register      — channel.cc:364-380
 *   Phase 2 sleep         — channel.cc:392-394
 *   Phase 3 cleanup       — channel.cc:398-417
 *   INT_MIN check + retry — channel.cc:423-428
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS S1, S2   \* Two slots
CONSTANTS C1, C2   \* Two channels

Slots == {S1, S2}
Chans == {C1, C2}

VARIABLES
    slot_chan,       \* [Slots -> Chans]: current slot→channel mapping
    chan_mu,         \* [Chans -> {"none","waiter","swapper","closer"}]
    waiter_reg,     \* [Chans -> BOOLEAN]: waiter registered on channel
    pinned,         \* [Chans -> BOOLEAN]: waiter has pinned this channel
    alive,          \* [Chans -> Int]: alive_ counter
    endpt_dead,     \* [Chans -> BOOLEAN]: writer endpoint dead
    channel_live,   \* [Chans -> BOOLEAN]: channel not deleted
    alt_state,      \* "idle" | "waiting" | "claimed"
    on_queue,       \* BOOLEAN: waiter on run queue
    signal,         \* "none" | "swap" | "death"
    chanop_chan,    \* [Slots -> Chans]: waiter's resolved channel per slot
    resolved,       \* BOOLEAN: ghost — waiter resolved since last swap signal
    pc_waiter,      \* waiter program counter
    pc_swapper,     \* swapper program counter
    pc_closer       \* closer program counter

vars == <<slot_chan, chan_mu, waiter_reg, pinned, alive, endpt_dead,
          channel_live, alt_state, on_queue, signal, chanop_chan,
          resolved, pc_waiter, pc_swapper, pc_closer>>

(*******************************************************************************
 * Initial state.
 ******************************************************************************)
Init ==
    /\ slot_chan    = [s \in Slots |-> IF s = S1 THEN C1 ELSE C2]
    /\ chan_mu      = [c \in Chans |-> "none"]
    /\ waiter_reg   = [c \in Chans |-> FALSE]
    /\ pinned       = [c \in Chans |-> FALSE]
    /\ alive        = [c \in Chans |-> 2]
    /\ endpt_dead   = [c \in Chans |-> FALSE]
    /\ channel_live = [c \in Chans |-> TRUE]
    /\ alt_state    = "idle"
    /\ on_queue     = TRUE
    /\ signal       = "none"
    /\ chanop_chan  = [s \in Slots |-> IF s = S1 THEN C1 ELSE C2]
    /\ resolved     = FALSE
    /\ pc_waiter    = "resolve"
    /\ pc_swapper   = "start"
    /\ pc_closer    = "start"

(*******************************************************************************
 * WAITER ACTIONS
 *
 * Full prialt lifecycle: resolve → lock → register → pin → sleep →
 * wake → deregister → unpin → check signal → retry/done.
 ******************************************************************************)

(* Phase 1: resolve slot→channel through Slot indirection.
 * Each chanop re-reads its Slot's channel pointer.
 *
 * Code: channel.cc:226-239 (retry: label, re-resolution loop)
 *
 * TLA:SwapWaiterRetry.WaiterResolve *)
WaiterResolve ==
    /\ pc_waiter = "resolve"
    /\ chanop_chan' = slot_chan
    /\ resolved' = TRUE
    /\ pc_waiter' = "lock"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   endpt_dead, channel_live, alt_state, on_queue,
                   signal, pc_swapper, pc_closer>>

(* Lock all channels in ID order (C1 before C2).
 *
 * Code: channel.cc:285-288 lock_all()
 *
 * TLA:SwapWaiterRetry.WaiterLock *)
WaiterLock ==
    /\ pc_waiter = "lock"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "waiter"]
    /\ pc_waiter' = "register"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_swapper, pc_closer>>

(* Phase 2: register on all channels and set alt_state=WAITING.
 * No match found during scan (simplified — scan logic is in
 * MultiChannelAlt spec).
 *
 * Code: channel.cc:364-372
 *
 * TLA:SwapWaiterRetry.WaiterRegister *)
WaiterRegister ==
    /\ pc_waiter = "register"
    /\ alt_state' = "waiting"
    /\ waiter_reg' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "pin"
    /\ UNCHANGED <<slot_chan, chan_mu, pinned, alive, endpt_dead,
                   channel_live, on_queue, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

(* Pin all channels: alive_.fetch_add(1).
 *
 * Code: channel.cc:378-380
 *
 * TLA:SwapWaiterRetry.WaiterPin *)
WaiterPin ==
    /\ pc_waiter = "pin"
    /\ alive' = [c \in Chans |-> alive[c] + 1]
    /\ pinned' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "sleep"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_swapper, pc_closer>>

(* Sleep: unlock all, context-switch out.
 *
 * Code: channel.cc:392-394
 *
 * TLA:SwapWaiterRetry.WaiterSleep *)
WaiterSleep ==
    /\ pc_waiter = "sleep"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ on_queue' = FALSE
    /\ pc_waiter' = "asleep"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

(* Phase 3: wake up (on_queue=TRUE), re-acquire all locks.
 *
 * Code: channel.cc:398
 *
 * TLA:SwapWaiterRetry.WaiterWakeLock *)
WaiterWakeLock ==
    /\ pc_waiter = "asleep"
    /\ on_queue = TRUE
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "waiter"]
    /\ pc_waiter' = "deregister"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_swapper, pc_closer>>

(* Deregister from all channels, set alt_state=IDLE, unlock.
 *
 * Code: channel.cc:399-406, 419
 *
 * TLA:SwapWaiterRetry.WaiterDeregister *)
WaiterDeregister ==
    /\ pc_waiter = "deregister"
    /\ waiter_reg' = [c \in Chans |-> FALSE]
    /\ alt_state' = "idle"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ pc_waiter' = "unpin"
    /\ UNCHANGED <<slot_chan, pinned, alive, endpt_dead,
                   channel_live, on_queue, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

(* Unpin all channels: alive_.fetch_sub(1).  Delete if alive→0.
 *
 * Code: channel.cc:410-417
 *
 * TLA:SwapWaiterRetry.WaiterUnpin *)
WaiterUnpin ==
    /\ pc_waiter = "unpin"
    /\ alive' = [c \in Chans |->
                    IF pinned[c] THEN alive[c] - 1 ELSE alive[c]]
    /\ channel_live' = [c \in Chans |->
                    IF pinned[c] /\ alive[c] = 1
                    THEN FALSE ELSE channel_live[c]]
    /\ pinned' = [c \in Chans |-> FALSE]
    /\ pc_waiter' = "check_signal"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, endpt_dead,
                   alt_state, on_queue, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

(* Check signal: if INT_MIN (swap), retry with re-resolution.
 * Otherwise, return result.
 *
 * Code: channel.cc:423-428
 *
 * TLA:SwapWaiterRetry.WaiterCheckSignal *)
WaiterCheckSignal ==
    /\ pc_waiter = "check_signal"
    /\ IF signal = "swap"
       THEN /\ signal' = "none"
            /\ resolved' = FALSE   \* Ghost: mark as needing re-resolution
            /\ pc_waiter' = "resolve"
       ELSE /\ pc_waiter' = "done"
            /\ UNCHANGED <<signal, resolved>>
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   endpt_dead, channel_live, alt_state, on_queue,
                   chanop_chan, pc_swapper, pc_closer>>

(*******************************************************************************
 * SWAPPER ACTIONS
 *
 * swap_slots atomically exchanges slot→channel pointers and wakes
 * all registered waiters with signal=INT_MIN.
 ******************************************************************************)

(* Lock both channels in ID order.
 *
 * Code: channel.cc:588-591
 *
 * TLA:SwapWaiterRetry.SwapLock *)
SwapLock ==
    /\ pc_swapper = "start"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "swapper"]
    /\ pc_swapper' = "exchange"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

(* Exchange slot→channel pointers.  Both locks held.
 * Verification step (channel.cc:597-604) is elided because this
 * model has a single swapper, so verify always succeeds.
 *
 * Code: channel.cc:612-623
 *
 * TLA:SwapWaiterRetry.SwapExchange *)
SwapExchange ==
    /\ pc_swapper = "exchange"
    /\ slot_chan' = [s \in Slots |->
                       IF s = S1 THEN slot_chan[S2] ELSE slot_chan[S1]]
    /\ pc_swapper' = "wake"
    /\ UNCHANGED <<chan_mu, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

(* Wake all registered waiters via CAS WAITING→CLAIMED with
 * signal=INT_MIN.
 *
 * Code: channel.cc:120-136 (wake_all_for_swap)
 *
 * TLA:SwapWaiterRetry.SwapWake *)
SwapWake ==
    /\ pc_swapper = "wake"
    /\ IF \E c \in Chans : waiter_reg[c] /\ alt_state = "waiting"
       THEN /\ alt_state' = "claimed"
            /\ on_queue' = TRUE
            /\ signal' = "swap"
       ELSE UNCHANGED <<alt_state, on_queue, signal>>
    /\ pc_swapper' = "unlock"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   endpt_dead, channel_live, chanop_chan, resolved,
                   pc_waiter, pc_closer>>

(* Unlock both channels.
 *
 * Code: channel.cc:633
 *
 * TLA:SwapWaiterRetry.SwapUnlock *)
SwapUnlock ==
    /\ pc_swapper = "unlock"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ pc_swapper' = "done"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

(*******************************************************************************
 * CLOSER ACTIONS
 *
 * Closer kills the writer endpoint on C1, concurrent with swap/waiter.
 ******************************************************************************)

(* Mark C1's writer endpoint as dead.
 *
 * Code: channel.cc:550-556
 *
 * TLA:SwapWaiterRetry.CloserDecRef *)
CloserDecRef ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "lock"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_swapper>>

(* Lock C1's mutex.
 *
 * Code: channel.cc:194
 *
 * TLA:SwapWaiterRetry.CloserLock *)
CloserLock ==
    /\ pc_closer = "lock"
    /\ channel_live[C1]
    /\ chan_mu[C1] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "closer"]
    /\ pc_closer' = "on_death"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_swapper>>

(* Wake registered waiter via CAS WAITING→CLAIMED, dec alive_.
 *
 * Code: channel.cc:143-179 (on_endpoint_death_locked)
 *
 * TLA:SwapWaiterRetry.CloserOnDeath *)
CloserOnDeath ==
    /\ pc_closer = "on_death"
    /\ IF waiter_reg[C1] /\ alt_state = "waiting"
       THEN /\ alt_state' = "claimed"
            /\ on_queue' = TRUE
            /\ signal' = "death"
       ELSE UNCHANGED <<alt_state, on_queue, signal>>
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
    /\ alive' = [alive EXCEPT ![C1] = @ - 1]
    /\ channel_live' = [channel_live EXCEPT ![C1] =
                            IF alive[C1] = 1 THEN FALSE ELSE @]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, endpt_dead,
                   chanop_chan, resolved, pc_waiter, pc_swapper>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WaiterResolve
    \/ WaiterLock
    \/ WaiterRegister
    \/ WaiterPin
    \/ WaiterSleep
    \/ WaiterWakeLock
    \/ WaiterDeregister
    \/ WaiterUnpin
    \/ WaiterCheckSignal
    \/ SwapLock
    \/ SwapExchange
    \/ SwapWake
    \/ SwapUnlock
    \/ CloserDecRef
    \/ CloserLock
    \/ CloserOnDeath

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ \A s \in Slots : slot_chan[s] \in Chans
    /\ \A c \in Chans : chan_mu[c] \in {"none", "waiter", "swapper", "closer"}
    /\ \A c \in Chans : waiter_reg[c] \in BOOLEAN
    /\ \A c \in Chans : pinned[c] \in BOOLEAN
    /\ \A c \in Chans : alive[c] \in 0..3
    /\ \A c \in Chans : endpt_dead[c] \in BOOLEAN
    /\ \A c \in Chans : channel_live[c] \in BOOLEAN
    /\ alt_state \in {"idle", "waiting", "claimed"}
    /\ on_queue \in BOOLEAN
    /\ signal \in {"none", "swap", "death"}
    /\ \A s \in Slots : chanop_chan[s] \in Chans
    /\ resolved \in BOOLEAN
    /\ pc_waiter \in {"resolve", "lock", "register", "pin", "sleep",
                       "asleep", "deregister", "unpin", "check_signal",
                       "done"}
    /\ pc_swapper \in {"start", "exchange", "wake", "unlock", "done"}
    /\ pc_closer \in {"start", "lock", "on_death", "done"}

(* No stale channel access: when the waiter is about to register,
 * it must have resolved slot→channel since the last swap signal.
 * In the correct protocol, WaiterCheckSignal on swap always goes
 * to "resolve" first, so resolved=TRUE at "register".  *)
NoStaleChannelAccess ==
    pc_waiter = "register" => resolved

(* Pin prevents delete: while asleep with pinned channels, those
 * channels remain live. *)
PinPreventsDelete ==
    pc_waiter = "asleep"
        => \A c \in Chans : pinned[c] => channel_live[c]

(* alive counters never go negative. *)
AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

====
