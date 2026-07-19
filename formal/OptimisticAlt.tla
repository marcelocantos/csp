---- MODULE OptimisticAlt ----
(*******************************************************************************
 * Models the 🎯T35 optimistic multi-channel phase-1 scan:
 *
 *   For count > 1, try each chanop under its OWN channel lock (not lock_all).
 *   On match: return holding only that channel's lock (alt_end unlocks it).
 *   On miss: fall through to the classical lock_all re-scan before sleep
 *            (preserves lost-wakeup freedom and dead-data deferral).
 *
 * Motivation: fan-in (alt/8ch) at high proc counts was bound by lock_all of
 * K channels while K writers hammered the same locks. The always-ready path
 * almost always matches in phase 1; locking one channel at a time removes
 * the multi-lock contention without changing rendezvous semantics.
 *
 * Participants:
 *   Waiter  — multi-channel alt over C1 then C2 (priority order).
 *   Peer1   — ready waiter on C1.
 *   Peer2   — ready waiter on C2 (only reachable if C1 has no peer).
 *   Closer  — kills C1's writer endpoint mid-scan (dead-data deferral).
 *
 * Safety properties:
 *   - AtMostOneLockOnMatch: a phase-1 match holds exactly one channel lock.
 *   - DeadDataDeferral: if C1 is dead but C2 has a peer, prefer the peer
 *     (scan continues after recording dead_data; lock_all re-scan still
 *     sees C2).
 *   - NoLostWakeup: sleep is entered only under lock_all after a re-scan.
 *   - MatchExclusive: at most one of peer1/peer2 is claimed.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS C1, C2
Chans == {C1, C2}

VARIABLES
    endpt_dead,      \* [Chans -> BOOLEAN]
    chan_mu,         \* [Chans -> {"none","waiter","peer","closer"}]
    peer1_waiting,   \* BOOLEAN: peer registered on C1
    peer2_waiting,   \* BOOLEAN: peer registered on C2
    peer1_claimed,
    peer2_claimed,
    dead_data,       \* BOOLEAN: deferred dead on a data arm
    signal,          \* "none" | "match_c1" | "match_c2" | "dead_c1" | "sleeping"
    locks_held,      \* 0..2: number of channel locks waiter currently holds
    scanned_c2,      \* ghost: waiter looked at C2 in this attempt
    used_optimistic, \* ghost: matched via single-lock path
    used_lock_all,   \* ghost: entered lock_all re-scan
    pc_waiter,
    pc_closer

vars == <<endpt_dead, chan_mu, peer1_waiting, peer2_waiting,
          peer1_claimed, peer2_claimed, dead_data, signal, locks_held,
          scanned_c2, used_optimistic, used_lock_all, pc_waiter, pc_closer>>

Init ==
    /\ endpt_dead     = [c \in Chans |-> FALSE]
    /\ chan_mu        = [c \in Chans |-> "none"]
    /\ peer1_waiting  = TRUE     \* default: C1 has a ready peer (fan-in shape)
    /\ peer2_waiting  = TRUE
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

(*******************************************************************************
 * CLOSER — may kill C1 while waiter scans optimistically.
 ******************************************************************************)

CloserKillC1 ==
    /\ pc_closer = "start"
    /\ endpt_dead' = [endpt_dead EXCEPT ![C1] = TRUE]
    /\ pc_closer' = "done"
    /\ UNCHANGED <<chan_mu, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, locks_held,
                   scanned_c2, used_optimistic, used_lock_all, pc_waiter>>

(*******************************************************************************
 * OPTIMISTIC PHASE 1 — one lock at a time.
 ******************************************************************************)

(* Lock C1 alone. *)
OptLockC1 ==
    /\ pc_waiter = "opt_c1"
    /\ chan_mu[C1] = "none"
    /\ chan_mu' = [chan_mu EXCEPT ![C1] = "waiter"]
    /\ locks_held' = 1
    /\ pc_waiter' = "opt_scan_c1"
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, scanned_c2,
                   used_optimistic, used_lock_all, pc_closer>>

(* Scan C1 under C1's lock only. *)
OptScanC1 ==
    /\ pc_waiter = "opt_scan_c1"
    /\ IF endpt_dead[C1]
       THEN \* data arm on dead channel: defer, unlock, continue
            /\ dead_data' = TRUE
            /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
            /\ locks_held' = 0
            /\ pc_waiter' = "opt_c2"
            /\ UNCHANGED <<peer1_claimed, peer2_claimed, signal,
                           used_optimistic>>
       ELSE IF peer1_waiting /\ ~peer1_claimed
            THEN \* MATCH under single lock
                 /\ peer1_claimed' = TRUE
                 /\ signal' = "match_c1"
                 /\ used_optimistic' = TRUE
                 /\ pc_waiter' = "done"
                 /\ UNCHANGED <<dead_data, chan_mu, locks_held,
                                peer2_claimed>>
            ELSE \* no peer: unlock, try C2
                 /\ chan_mu' = [chan_mu EXCEPT ![C1] = "none"]
                 /\ locks_held' = 0
                 /\ pc_waiter' = "opt_c2"
                 /\ UNCHANGED <<dead_data, peer1_claimed, peer2_claimed,
                                signal, used_optimistic>>
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, scanned_c2,
                   used_lock_all, pc_closer>>

(* Lock C2 alone. *)
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

(* Scan C2 under C2's lock only. *)
OptScanC2 ==
    /\ pc_waiter = "opt_scan_c2"
    /\ IF endpt_dead[C2]
       THEN /\ dead_data' = TRUE
            /\ chan_mu' = [chan_mu EXCEPT ![C2] = "none"]
            /\ locks_held' = 0
            /\ pc_waiter' = "fallthrough"
            /\ UNCHANGED <<peer1_claimed, peer2_claimed, signal,
                           used_optimistic>>
       ELSE IF peer2_waiting /\ ~peer2_claimed
            THEN /\ peer2_claimed' = TRUE
                 /\ signal' = "match_c2"
                 /\ used_optimistic' = TRUE
                 /\ pc_waiter' = "done"
                 /\ UNCHANGED <<dead_data, chan_mu, locks_held,
                                peer1_claimed>>
            ELSE /\ chan_mu' = [chan_mu EXCEPT ![C2] = "none"]
                 /\ locks_held' = 0
                 /\ pc_waiter' = "fallthrough"
                 /\ UNCHANGED <<dead_data, peer1_claimed, peer2_claimed,
                                signal, used_optimistic>>
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, scanned_c2,
                   used_lock_all, pc_closer>>

(*******************************************************************************
 * FALL-THROUGH: classical lock_all re-scan before sleep.
 * Entered only when optimistic phase found no match.
 ******************************************************************************)

LockAll ==
    /\ pc_waiter = "fallthrough"
    /\ chan_mu[C1] = "none"
    /\ chan_mu[C2] = "none"
    /\ chan_mu' = [c \in Chans |-> "waiter"]
    /\ locks_held' = 2
    /\ used_lock_all' = TRUE
    /\ pc_waiter' = "rescan"
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting, peer1_claimed,
                   peer2_claimed, dead_data, signal, scanned_c2,
                   used_optimistic, pc_closer>>

(* Re-scan under lock_all. Prefer ready peers over dead_data. *)
Rescan ==
    /\ pc_waiter = "rescan"
    /\ IF peer1_waiting /\ ~peer1_claimed /\ ~endpt_dead[C1]
       THEN /\ peer1_claimed' = TRUE
            /\ signal' = "match_c1"
            /\ pc_waiter' = "done"
            /\ UNCHANGED <<peer2_claimed, dead_data, chan_mu, locks_held>>
       ELSE IF peer2_waiting /\ ~peer2_claimed /\ ~endpt_dead[C2]
            THEN /\ peer2_claimed' = TRUE
                 /\ signal' = "match_c2"
                 /\ scanned_c2' = TRUE
                 /\ pc_waiter' = "done"
                 /\ UNCHANGED <<peer1_claimed, dead_data, chan_mu, locks_held>>
            ELSE IF dead_data \/ endpt_dead[C1] \/ endpt_dead[C2]
                 THEN /\ signal' = "dead_c1"
                      /\ chan_mu' = [c \in Chans |-> "none"]
                      /\ locks_held' = 0
                      /\ pc_waiter' = "done"
                      /\ UNCHANGED <<peer1_claimed, peer2_claimed, dead_data,
                                     scanned_c2>>
                 ELSE \* no peer, not dead → would sleep (register under lock_all)
                      /\ signal' = "sleeping"
                      /\ pc_waiter' = "done"
                      \* locks still held (registration path); model as held
                      /\ UNCHANGED <<peer1_claimed, peer2_claimed, dead_data,
                                     chan_mu, locks_held, scanned_c2>>
    /\ UNCHANGED <<endpt_dead, peer1_waiting, peer2_waiting,
                   used_optimistic, used_lock_all, pc_closer>>

(*******************************************************************************
 * SPEC
 ******************************************************************************)

Next ==
    \/ CloserKillC1
    \/ OptLockC1
    \/ OptScanC1
    \/ OptLockC2
    \/ OptScanC2
    \/ LockAll
    \/ Rescan

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ \A c \in Chans : endpt_dead[c] \in BOOLEAN
    /\ \A c \in Chans : chan_mu[c] \in {"none", "waiter", "peer", "closer"}
    /\ peer1_waiting \in BOOLEAN
    /\ peer2_waiting \in BOOLEAN
    /\ peer1_claimed \in BOOLEAN
    /\ peer2_claimed \in BOOLEAN
    /\ dead_data \in BOOLEAN
    /\ signal \in {"none", "match_c1", "match_c2", "dead_c1", "sleeping"}
    /\ locks_held \in 0..2
    /\ scanned_c2 \in BOOLEAN
    /\ used_optimistic \in BOOLEAN
    /\ used_lock_all \in BOOLEAN
    /\ pc_waiter \in {"opt_c1", "opt_scan_c1", "opt_c2", "opt_scan_c2",
                       "fallthrough", "rescan", "done"}
    /\ pc_closer \in {"start", "done"}

(* A successful optimistic match holds exactly one channel lock. *)
AtMostOneLockOnOptimisticMatch ==
    (pc_waiter = "done" /\ used_optimistic)
        => (locks_held = 1 /\ signal \in {"match_c1", "match_c2"})

(* Sleep is entered only after lock_all (no sleep from optimistic path). *)
SleepOnlyAfterLockAll ==
    (pc_waiter = "done" /\ signal = "sleeping")
        => (used_lock_all /\ locks_held = 2)

(* Dead-data must not fire if a peer was claimable on a later channel.
 * Approximates MultiChannelAlt.DeadDataDeferral for the optimistic path:
 * if we report dead and peer2 was waiting unclaimed, we must have scanned C2
 * (either optimistically or via lock_all). *)
DeadDataDeferral ==
    (pc_waiter = "done" /\ signal = "dead_c1"
        /\ peer2_waiting /\ ~peer2_claimed)
        => scanned_c2

(* At most one peer claimed. *)
MatchExclusive ==
    ~(peer1_claimed /\ peer2_claimed)

(* locks_held agrees with chan_mu. *)
LocksConsistent ==
    locks_held = Cardinality({c \in Chans : chan_mu[c] = "waiter"})

====
