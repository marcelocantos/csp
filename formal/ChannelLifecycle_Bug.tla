---- MODULE ChannelLifecycle_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates what happens without the alive_ two-phase
 * delete guard.
 *
 * In this version, each closer unconditionally deletes the channel after
 * releasing mu_, instead of using alive_.fetch_sub() to determine which
 * one should delete. Both closers run concurrently.
 *
 * The dangerous interleaving:
 *   CloserW: acquire mu_, release mu_, delete channel (channel_live=FALSE)
 *   CloserR: acquire mu_, release mu_, access alive_ on freed memory!
 *
 * Expected result: TLC finds a NoUseAfterFree violation — the second
 * closer accesses channel state (to do its delete) after the first closer
 * has already freed it.
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    alive,          \* Integer: channel alive counter
    channel_live,   \* Boolean: channel not yet deleted
    ch_mu,          \* "none" | "W" | "R": who holds mutex
    pc_cw,          \* CloserW program counter
    pc_cr           \* CloserR program counter

vars == <<alive, channel_live, ch_mu, pc_cw, pc_cr>>

Init ==
    /\ alive = 2
    /\ channel_live = TRUE
    /\ ch_mu = "none"
    /\ pc_cw = "start"
    /\ pc_cr = "start"

(*******************************************************************************
 * CLOSER W ACTIONS
 ******************************************************************************)

(* CloserW acquires mu_.
 * Code: channel.cc:124 *)
CloserWAcquire ==
    /\ pc_cw = "start"
    /\ ch_mu = "none"
    /\ ch_mu' = "W"
    /\ pc_cw' = "waking"
    /\ UNCHANGED <<alive, channel_live, pc_cr>>

(* CloserW releases mu_ (skipping wake step for simplicity).
 * Code: channel.cc:149 *)
CloserWReleaseMu ==
    /\ pc_cw = "waking"
    /\ ch_mu' = "none"
    /\ pc_cw' = "dec_alive"
    /\ UNCHANGED <<alive, channel_live, pc_cr>>

(* BUG: CloserW always deletes — no alive_ coordination.
 * In the correct code, only the thread whose fetch_sub returns 1 deletes.
 * Here, every closer deletes unconditionally. *)
CloserWDecAlive ==
    /\ pc_cw = "dec_alive"
    /\ alive' = alive - 1
    /\ channel_live' = FALSE    \* BUG: unconditional delete
    /\ pc_cw' = "done_cw"
    /\ UNCHANGED <<ch_mu, pc_cr>>

(*******************************************************************************
 * CLOSER R ACTIONS
 ******************************************************************************)

CloserRAcquire ==
    /\ pc_cr = "start"
    /\ ch_mu = "none"
    /\ ch_mu' = "R"
    /\ pc_cr' = "waking"
    /\ UNCHANGED <<alive, channel_live, pc_cw>>

CloserRReleaseMu ==
    /\ pc_cr = "waking"
    /\ ch_mu' = "none"
    /\ pc_cr' = "dec_alive"
    /\ UNCHANGED <<alive, channel_live, pc_cw>>

(* BUG: CloserR always deletes — no alive_ coordination. *)
CloserRDecAlive ==
    /\ pc_cr = "dec_alive"
    /\ alive' = alive - 1
    /\ channel_live' = FALSE    \* BUG: unconditional delete
    /\ pc_cr' = "done_cr"
    /\ UNCHANGED <<ch_mu, pc_cw>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ CloserWAcquire
    \/ CloserWReleaseMu
    \/ CloserWDecAlive
    \/ CloserRAcquire
    \/ CloserRReleaseMu
    \/ CloserRDecAlive

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ alive \in 0..2
    /\ channel_live \in BOOLEAN
    /\ ch_mu \in {"none", "W", "R"}
    /\ pc_cw \in {"start", "waking", "dec_alive", "done_cw"}
    /\ pc_cr \in {"start", "waking", "dec_alive", "done_cr"}

(* This invariant should FAIL: the second closer is in "dec_alive" state
 * (about to access alive_ on the channel) but channel_live is already
 * FALSE because the first closer deleted it. This is a use-after-free. *)
NoUseAfterFree ==
    /\ (pc_cw = "dec_alive") => channel_live
    /\ (pc_cr = "dec_alive") => channel_live

====
