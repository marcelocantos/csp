---- MODULE OptimisticAlt_Bug ----
(*******************************************************************************
 * Bug variant: optimistic path reports dead_data immediately on C1 without
 * scanning C2 — the multi-channel dead-data-deferral regression.
 *
 * Expected TLC result: DeadDataDeferral violation when peer2 is waiting.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS C1, C2
Chans == {C1, C2}

VARIABLES
    endpt_dead, chan_mu, peer1_waiting, peer2_waiting,
    peer1_claimed, peer2_claimed, dead_data, signal, locks_held,
    scanned_c2, used_optimistic, used_lock_all, pc_waiter, pc_closer

vars == <<endpt_dead, chan_mu, peer1_waiting, peer2_waiting,
          peer1_claimed, peer2_claimed, dead_data, signal, locks_held,
          scanned_c2, used_optimistic, used_lock_all, pc_waiter, pc_closer>>

Init ==
    /\ endpt_dead     = [c \in Chans |-> FALSE]
    /\ chan_mu        = [c \in Chans |-> "none"]
    /\ peer1_waiting  = FALSE    \* no peer on C1
    /\ peer2_waiting  = TRUE     \* peer on C2 available
    /\ peer1_claimed  = FALSE
    /\ peer2_claimed  = FALSE
    /\ dead_data      = FALSE
    /\ signal         = "none"
    /\ locks_held     = 0
    /\ scanned_c2     = FALSE
    /\ used_optimistic = FALSE
    /\ used_lock_all  = FALSE
    /\ pc_waiter      = "opt_c1"
    /\ pc_closer      = "start"

\* C1 already dead at start of interesting race (or closer races in).
CloserKillC1 ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<chan_mu, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, locks_held,
                   scanned_c2, used_optimistic, used_lock_all, pc_waiter>>

OptLockC1 ==
    /\ pc_waiter = "opt_c1"
    /\ chan_mu[C1] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "waiter"]
    /\ locks_held' = 1
    /\ pc_waiter' = "opt_scan_c1"
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, scanned_c2,
                   used_optimistic, used_lock_all, pc_closer>>

(* BUG: on dead C1, return immediately without scanning C2. *)
OptScanC1 ==
    /\ pc_waiter = "opt_scan_c1"
    /\ IF endpt_dead[C1]
       THEN /\ dead_data' = TRUE
            /\ signal' = "dead_c1"
            /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
            /\ locks_held' = 0
            /\ pc_waiter' = "done"          \* BUG: skip C2
            /\ UNCHANGED <<peer1_claimed, peer2_claimed, used_optimistic>>
       ELSE IF peer1_waiting /\ ~peer1_claimed
            THEN /\ peer1_claimed' = TRUE
                 /\ signal' = "match_c1"
                 /\ used_optimistic' = TRUE
                 /\ pc_waiter' = "done"
                 /\ UNCHANGED <<dead_data, chan_mu, locks_held, peer2_claimed>>
            ELSE /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
                 /\ locks_held' = 0
                 /\ pc_waiter' = "opt_c2"
                 /\ UNCHANGED <<dead_data, peer1_claimed, peer2_claimed,
                                signal, used_optimistic>>
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, scanned_c2,
                   used_lock_all, pc_closer>>

OptLockC2 ==
    /\ pc_waiter = "opt_c2"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C2] = "waiter"]
    /\ locks_held' = 1
    /\ scanned_c2' = TRUE
    /\ pc_waiter' = "opt_scan_c2"
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, used_optimistic,
                   used_lock_all, pc_closer>>

OptScanC2 ==
    /\ pc_waiter = "opt_scan_c2"
    /\ IF peer2_waiting /\ ~peer2_claimed
       THEN /\ peer2_claimed' = TRUE
            /\ signal' = "match_c2"
            /\ used_optimistic' = TRUE
            /\ pc_waiter' = "done"
            /\ UNCHANGED <<dead_data, chan_mu, locks_held, peer1_claimed>>
       ELSE /\ chan_mu' = [chan_mu EXCEPT ![C2] = "none"]
            /\ locks_held' = 0
            /\ pc_waiter' = "done"
            /\ UNCHANGED <<dead_data, peer1_claimed, peer2_claimed,
                           signal, used_optimistic>>
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, scanned_c2,
                   used_lock_all, pc_closer>>

Next ==
    \/ CloserKillC1
    \/ OptLockC1
    \/ OptScanC1
    \/ OptLockC2
    \/ OptScanC2

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ signal \in {"none", "match_c1", "match_c2", "dead_c1", "sleeping"}
    /\ scanned_c2 \in BOOLEAN
    /\ pc_waiter \in {"opt_c1", "opt_scan_c1", "opt_c2", "opt_scan_c2", "done"}

DeadDataDeferral ==
    (pc_waiter = "done" /\ signal = "dead_c1"
        /\ peer2_waiting /\ ~peer2_claimed)
        => scanned_c2

====
