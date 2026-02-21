---- MODULE SwapWaiterRetry_Bug ----
(*******************************************************************************
 * Bug variant of SwapWaiterRetry: skips re-resolution after swap.
 *
 * When the waiter is woken by swap (signal=INT_MIN), the correct code
 * jumps to the retry: label which re-resolves slot→channel through
 * Slot indirection before re-scanning.  This bug variant skips the
 * re-resolution and goes directly to locking, leaving chanop_chan
 * stale (pointing to pre-swap channels).
 *
 * Expected TLC result: NoStaleChannelAccess violation.
 *
 * Concrete trace:
 *   1. WaiterResolve: chanop_chan = {S1→C1, S2→C2}
 *   2. WaiterLock, WaiterRegister, WaiterPin, WaiterSleep
 *   3. SwapLock, SwapExchange: slot_chan = {S1→C2, S2→C1}
 *   4. SwapWake: waiter claimed with signal="swap"
 *   5. SwapUnlock
 *   6. WaiterWakeLock, WaiterDeregister, WaiterUnpin
 *   7. WaiterCheckSignal: signal="swap" → BUG: go to "lock" not "resolve"
 *   8. WaiterLock → WaiterRegister: resolved=FALSE → VIOLATION
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS S1, S2
CONSTANTS C1, C2

Slots == {S1, S2}
Chans == {C1, C2}

VARIABLES
    slot_chan, chan_mu, waiter_reg, pinned, alive, endpt_dead,
    channel_live, alt_state, on_queue, signal, chanop_chan,
    resolved, pc_waiter, pc_swapper, pc_closer

vars == <<slot_chan, chan_mu, waiter_reg, pinned, alive, endpt_dead,
          channel_live, alt_state, on_queue, signal, chanop_chan,
          resolved, pc_waiter, pc_swapper, pc_closer>>

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

\* ---- WAITER ----

WaiterResolve ==
    /\ pc_waiter = "resolve"
    /\ chanop_chan' = slot_chan
    /\ resolved' = TRUE
    /\ pc_waiter' = "lock"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   endpt_dead, channel_live, alt_state, on_queue,
                   signal, pc_swapper, pc_closer>>

WaiterLock ==
    /\ pc_waiter = "lock"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "waiter"]
    /\ pc_waiter' = "register"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_swapper, pc_closer>>

WaiterRegister ==
    /\ pc_waiter = "register"
    /\ alt_state' = "waiting"
    /\ waiter_reg' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "pin"
    /\ UNCHANGED <<slot_chan, chan_mu, pinned, alive, endpt_dead,
                   channel_live, on_queue, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

WaiterPin ==
    /\ pc_waiter = "pin"
    /\ alive' = [c \in Chans |-> alive[c] + 1]
    /\ pinned' = [c \in Chans |-> TRUE]
    /\ pc_waiter' = "sleep"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_swapper, pc_closer>>

WaiterSleep ==
    /\ pc_waiter = "sleep"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ on_queue' = FALSE
    /\ pc_waiter' = "asleep"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

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

WaiterDeregister ==
    /\ pc_waiter = "deregister"
    /\ waiter_reg' = [c \in Chans |-> FALSE]
    /\ alt_state' = "idle"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ pc_waiter' = "unpin"
    /\ UNCHANGED <<slot_chan, pinned, alive, endpt_dead,
                   channel_live, on_queue, signal, chanop_chan,
                   resolved, pc_swapper, pc_closer>>

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

(* BUG: on swap signal, go to "lock" instead of "resolve".
 * This skips re-resolution, leaving chanop_chan stale. *)
WaiterCheckSignal ==
    /\ pc_waiter = "check_signal"
    /\ IF signal = "swap"
       THEN /\ signal' = "none"
            /\ resolved' = FALSE
            /\ pc_waiter' = "lock"  \* BUG: should be "resolve"
       ELSE /\ pc_waiter' = "done"
            /\ UNCHANGED <<signal, resolved>>
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   endpt_dead, channel_live, alt_state, on_queue,
                   chanop_chan, pc_swapper, pc_closer>>

\* ---- SWAPPER ----

SwapLock ==
    /\ pc_swapper = "start"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "swapper"]
    /\ pc_swapper' = "exchange"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

SwapExchange ==
    /\ pc_swapper = "exchange"
    /\ slot_chan' = [s \in Slots |->
                       IF s = S1 THEN slot_chan[S2] ELSE slot_chan[S1]]
    /\ pc_swapper' = "wake"
    /\ UNCHANGED <<chan_mu, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

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

SwapUnlock ==
    /\ pc_swapper = "unlock"
    /\ chan_mu' = [c \in Chans |-> "none"]
    /\ pc_swapper' = "done"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_closer>>

\* ---- CLOSER ----

CloserDecRef ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "lock"
    /\ UNCHANGED <<slot_chan, chan_mu, waiter_reg, pinned, alive,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_swapper>>

CloserLock ==
    /\ pc_closer = "lock"
    /\ channel_live[C1]
    /\ chan_mu[C1] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "closer"]
    /\ pc_closer' = "on_death"
    /\ UNCHANGED <<slot_chan, waiter_reg, pinned, alive, endpt_dead,
                   channel_live, alt_state, on_queue, signal,
                   chanop_chan, resolved, pc_waiter, pc_swapper>>

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

\* ---- SPEC ----

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

\* ---- PROPERTIES ----

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

NoStaleChannelAccess ==
    pc_waiter = "register" => resolved

AliveNonNeg ==
    \A c \in Chans : alive[c] >= 0

====
