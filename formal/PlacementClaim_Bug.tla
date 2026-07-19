---- MODULE PlacementClaim_Bug ----
(*******************************************************************************
 * Buggy variant of PlacementClaim: each placer CHECKS placed_ and SETS
 * it in two separate steps (plain load + store) instead of one atomic
 * exchange.  Two placers can both read FALSE and both insert — the imp
 * lands on two run queues and is executed twice (AtMostOnce fails).
 *******************************************************************************)

EXTENDS Integers

CONSTANTS
    Placers

VARIABLES placed, queued, pc

vars == <<placed, queued, pc>>

Init ==
    /\ placed = FALSE
    /\ queued = 0
    /\ pc = [p \in Placers |-> "start"]

(* BUG: read placed_ ... *)
Check(p) ==
    /\ pc[p] = "start"
    /\ IF placed
       THEN pc' = [pc EXCEPT ![p] = "done"]
       ELSE pc' = [pc EXCEPT ![p] = "set"]
    /\ UNCHANGED <<placed, queued>>

(* ... then set it in a separate step. *)
Set(p) ==
    /\ pc[p] = "set"
    /\ placed' = TRUE
    /\ pc' = [pc EXCEPT ![p] = "insert"]
    /\ UNCHANGED queued

Insert(p) ==
    /\ pc[p] = "insert"
    /\ queued' = queued + 1
    /\ pc' = [pc EXCEPT ![p] = "done"]
    /\ UNCHANGED placed

Next == \E p \in Placers : Check(p) \/ Set(p) \/ Insert(p)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ placed \in BOOLEAN
    /\ queued \in 0..3
    /\ \A p \in Placers : pc[p] \in {"start", "set", "insert", "done"}

AtMostOnce == queued <= 1

ExactlyOnceAtEnd ==
    (\A p \in Placers : pc[p] = "done") => queued = 1

====
