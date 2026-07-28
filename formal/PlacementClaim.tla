---- MODULE PlacementClaim ----
(*******************************************************************************
 * Models the run-queue placement claim on Imp::placed_ (🎯T34 round 2).
 *
 * Up to three actors can race to queue one fully suspended imp:
 *   - the deferred-wake drain (drain_suspended saw SUSP_WAKE), and
 *   - late wakers whose schedule() observed SUSP_IDLE (e.g. a timer
 *     fire racing a cancel — duplicate wakes are legal).
 *
 * Historically the race was serialized by global_mu around the
 * next_/in_global_ checks — a global lock on every wake.  The claim
 * replaces it: placed_ is TRUE while the imp is in (or committed to)
 * a run queue; each placer performs ONE atomic exchange(TRUE) and only
 * the actor that observed FALSE inserts.  The imp itself clears
 * placed_ when it delinks on the suspend path — after the CheckWP
 * early-wake decision, so a woken-early imp never exposes a
 * FALSE window (see csp.cc do_switch()).
 *
 * Wakers can only reach the claim when suspend_state_ is SUSP_IDLE
 * (DrainSuspended.tla); this spec starts from that point: the imp is
 * suspended, delinked, placed_ = FALSE.
 *
 * PlacementClaim_Bug.tla replaces the exchange with a non-atomic
 * check-then-set; TLC finds the double placement.
 *******************************************************************************)

EXTENDS Integers

CONSTANTS
    Placers     \* set of racing placer IDs, e.g. {drain, w1, w2}

VARIABLES
    placed,     \* Boolean: the placed_ word
    queued,     \* 0..Cardinality(Placers): how many queue insertions happened
    pc          \* [Placers -> pc state]

vars == <<placed, queued, pc>>

Init ==
    /\ placed = FALSE
    /\ queued = 0
    /\ pc = [p \in Placers |-> "start"]

(* One atomic exchange(TRUE).  Winner (saw FALSE) goes on to insert;
 * loser is done — the winner is committed to placing.
 * TLA:PlacementClaim.Claim *)
Claim(p) ==
    /\ pc[p] = "start"
    /\ IF placed
       THEN pc' = [pc EXCEPT ![p] = "done"]
       ELSE pc' = [pc EXCEPT ![p] = "insert"]
    /\ placed' = TRUE
    /\ UNCHANGED queued

(* The winning placer inserts the imp into a run queue (local under
 * run_mu, or global under global_mu — the queue's own lock protects
 * queue integrity; the claim protects exclusivity).
 * TLA:PlacementClaim.Insert *)
Insert(p) ==
    /\ pc[p] = "insert"
    /\ queued' = queued + 1
    /\ pc' = [pc EXCEPT ![p] = "done"]
    /\ UNCHANGED placed

Next == \E p \in Placers : Claim(p) \/ Insert(p)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 *******************************************************************************)

TypeOK ==
    /\ placed \in BOOLEAN
    /\ queued \in 0..3
    /\ \A p \in Placers : pc[p] \in {"start", "insert", "done"}

(* The imp lands on at most one queue. *)
AtMostOnce == queued <= 1

(* No lost wake: once every placer has finished, someone inserted. *)
ExactlyOnceAtEnd ==
    (\A p \in Placers : pc[p] = "done") => queued = 1

====
