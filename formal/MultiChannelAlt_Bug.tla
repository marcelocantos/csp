---- MODULE MultiChannelAlt_Bug ----
(*******************************************************************************
 * Bug variant of MultiChannelAlt: removes dead-data deferral.
 *
 * Models the pre-1d6a732 behavior where a data chanop on a dead channel
 * returns immediately instead of deferring until after scanning for
 * ready peers on other channels.
 *
 * Expected TLC result: DeadDataDeferral violation.
 *
 * Concrete trace:
 *   1. PeerArrive: peer registers on C2
 *   2. CloserDecRef: C1's writer endpoint dies
 *   3. WaiterLockAll: waiter locks both channels
 *   4. WaiterScanC1: C1 is dead, data chanop → BUG: return immediately
 *      (instead of deferring and scanning C2 for the ready peer)
 *   5. signal = "dead_c1" but peer was available → DeadDataDeferral violated
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS C1, C2

Chans == {C1, C2}

VARIABLES
    endpt_dead, alive, channel_live, chan_mu, waiter_reg, pinned,
    alt_state, on_queue, signal, dead_data, peer_waiting,
    peer_claimed, scanned_c2, pc_waiter, pc_closer, pc_peer

vars == <<endpt_dead, alive, channel_live, chan_mu, waiter_reg, pinned,
          alt_state, on_queue, signal, dead_data, peer_waiting,
          peer_claimed, scanned_c2, pc_waiter, pc_closer, pc_peer>>

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

\* ---- PEER ----

PeerArrive ==
    /\ pc_peer = "idle"
    /\ chan_mu[C2] = "none"  \* peer registers under C2's mutex
    /\ peer_waiting' = TRUE
    /\ pc_peer' = "waiting"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, chan_mu, waiter_reg,
                   pinned, alt_state, on_queue, signal, dead_data,
                   peer_claimed, scanned_c2, pc_waiter, pc_closer>>

\* ---- WAITER ----

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

(* BUG: if C1 is dead, return immediately instead of deferring.
 * This is the pre-1d6a732 behavior. *)
WaiterScanC1 ==
    /\ pc_waiter = "scan_c1"
    /\ IF endpt_dead[C1]
       THEN \* BUG: fire immediately on dead data chanop
            /\ signal' = "dead_c1"
            /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
            /\ pc_waiter' = "done"
            /\ UNCHANGED dead_data
       ELSE /\ pc_waiter' = "scan_c2"
            /\ UNCHANGED <<signal, chan_mu, dead_data>>
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg,
                   pinned, alt_state, on_queue, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

WaiterScanC2 ==
    /\ pc_waiter = "scan_c2"
    /\ scanned_c2' = TRUE  \* ghost: record that we scanned C2
    /\ IF peer_waiting /\ ~peer_claimed /\ channel_live[C2]
       THEN /\ peer_claimed' = TRUE
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

WaiterMatchReturn ==
    /\ pc_waiter = "match_found"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
    /\ pc_waiter' = "done"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

WaiterPostScan ==
    /\ pc_waiter = "post_scan"
    /\ IF dead_data
       THEN /\ signal' = "dead_c1"
            /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none", ![C2] = "none"]
            /\ pc_waiter' = "done"
       ELSE /\ pc_waiter' = "done"
            /\ UNCHANGED <<signal, chan_mu>>
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_reg, pinned,
                   alt_state, on_queue, dead_data, peer_waiting,
                   peer_claimed, scanned_c2, pc_closer, pc_peer>>

\* ---- CLOSER ----

CloserDecRef ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<alive, channel_live, chan_mu, waiter_reg, pinned,
                   alt_state, on_queue, signal, dead_data,
                   peer_waiting, peer_claimed, scanned_c2,
                   pc_waiter, pc_peer>>

\* ---- SPEC ----

Next ==
    \/ PeerArrive
    \/ WaiterLockAll
    \/ WaiterScanC1
    \/ WaiterScanC2
    \/ WaiterMatchReturn
    \/ WaiterPostScan
    \/ CloserDecRef

Spec == Init /\ [][Next]_vars

\* ---- PROPERTIES ----

TypeOK ==
    /\ \A c \in Chans : endpt_dead[c] \in BOOLEAN
    /\ signal \in {"none", "match_c2", "dead_c1"}
    /\ peer_waiting \in BOOLEAN
    /\ peer_claimed \in BOOLEAN
    /\ scanned_c2 \in BOOLEAN
    /\ pc_waiter \in {"start", "scan_c1", "scan_c2", "match_found",
                       "post_scan", "done"}
    /\ pc_closer \in {"start", "done"}
    /\ pc_peer \in {"idle", "waiting"}

DeadDataDeferral ==
    (pc_waiter = "done" /\ signal = "dead_c1"
        /\ peer_waiting /\ ~peer_claimed)
        => scanned_c2

====
