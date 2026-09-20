---- MODULE CargoSelection ----
(* One move-only cargo competes with one control message in a priority alt.
   The eager variant moves Entity endpoint ownership into the send operand
   while constructing that operand. If control wins, destroying the unchosen
   operand loses the cargo's endpoints. The fixed variant borrows the local
   Cargo until the send commits, then moves it exactly once into the channel.
   In C++ that borrow is `out << csp::from(cargo)` (csp::deferred_send).

   Bounds: one cargo, one control selection, one queue slot, one receiver.
   This is an ownership/selection abstraction, not a proof of chan_op's C++
   implementation or of the choreography, scheduler, or exception handling.
   The live scenario must also exercise a control arriving during backpressure.
*)
EXTENDS Naturals

CONSTANT EagerMove
VARIABLES phase, actor, operand, queue, delivered, controlConsumed
vars == <<phase, actor, operand, queue, delivered, controlConsumed>>

Init == /\ phase = "idle"
        /\ actor = TRUE
        /\ operand = FALSE
        /\ queue = FALSE
        /\ delivered = FALSE
        /\ controlConsumed = FALSE

Prepare ==
    /\ phase = "idle"
    /\ phase' = "choosing"
    /\ operand' = IF EagerMove THEN actor ELSE FALSE
    /\ actor' = IF EagerMove THEN FALSE ELSE actor
    /\ UNCHANGED <<queue, delivered, controlConsumed>>

SelectControl ==
    /\ phase = "choosing" /\ ~controlConsumed
    /\ phase' = "idle"
    /\ controlConsumed' = TRUE
    /\ operand' = FALSE
    /\ UNCHANGED <<actor, queue, delivered>>

SelectSend ==
    /\ phase = "choosing"
    /\ phase' = "queued"
    /\ queue' = IF EagerMove THEN operand ELSE actor
    /\ actor' = FALSE
    /\ operand' = FALSE
    /\ UNCHANGED <<delivered, controlConsumed>>

Receive ==
    /\ phase = "queued" /\ queue
    /\ phase' = "complete"
    /\ queue' = FALSE
    /\ delivered' = TRUE
    /\ UNCHANGED <<actor, operand, controlConsumed>>

Next == Prepare \/ SelectControl \/ SelectSend \/ Receive
Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

TypeOK == /\ phase \in {"idle", "choosing", "queued", "complete"}
          /\ {actor, operand, queue, delivered, controlConsumed} \subseteq BOOLEAN
Owns(value) == IF value THEN 1 ELSE 0
UniqueCargo == Owns(actor) + Owns(operand) + Owns(queue) + Owns(delivered) = 1
EventuallyDelivered == <>delivered
====
