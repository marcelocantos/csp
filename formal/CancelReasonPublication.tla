---- MODULE CancelReasonPublication ----
(***************************************************************************
 Two callers compete to cancel one scope while a poller reads its reason.
 The claim elects one writer; publishing is a separate release store after
 writing the exception_ptr. A poller's acquire load must see publication
 before copying that pointer. Closing the channel follows publication.

 This is an interleaving model of the C++ release/acquire contract, not a
 model of the C++ memory model itself. All state is finite: two caller PCs,
 one reason, one reader, and Boolean flags. PublishBeforeWrite selects the
 original ordering for the companion _Bug model.
 ***************************************************************************
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS Cancellers, PublishBeforeWrite
VARIABLES claimed, published, reason, pc, readerPC, observed, closed

vars == <<claimed, published, reason, pc, readerPC, observed, closed>>

Init ==
    /\ claimed = FALSE
    /\ published = FALSE
    /\ reason = "none"
    /\ pc = [c \in Cancellers |-> "claim"]
    /\ readerPC = "poll"
    /\ observed = "none"
    /\ closed = FALSE

(* TLA:CancelReasonPublication.Claim *)
Claim(c) ==
    /\ pc[c] = "claim"
    /\ pc' = [pc EXCEPT ![c] = IF claimed THEN "done" ELSE "write"]
    /\ claimed' = TRUE
    /\ published' = (published \/ (PublishBeforeWrite /\ ~claimed))
    /\ UNCHANGED <<reason, readerPC, observed, closed>>

(* TLA:CancelReasonPublication.WriteReason *)
WriteReason(c) ==
    /\ pc[c] = "write"
    /\ reason' = c
    /\ pc' = [pc EXCEPT ![c] = IF PublishBeforeWrite THEN "close"
                              ELSE "publish"]
    /\ UNCHANGED <<claimed, published, readerPC, observed, closed>>

(* TLA:CancelReasonPublication.Publish *)
Publish(c) ==
    /\ pc[c] = "publish"
    /\ published' = TRUE
    /\ pc' = [pc EXCEPT ![c] = "close"]
    /\ UNCHANGED <<claimed, reason, readerPC, observed, closed>>

(* TLA:CancelReasonPublication.Close *)
Close(c) ==
    /\ pc[c] = "close"
    /\ closed' = TRUE
    /\ pc' = [pc EXCEPT ![c] = "done"]
    /\ UNCHANGED <<claimed, published, reason, readerPC, observed>>

(* The null-returning polling path does not touch reason.
   TLA:CancelReasonPublication.Poll *)
Poll ==
    /\ readerPC = "poll"
    /\ published
    /\ readerPC' = "read"
    /\ UNCHANGED <<claimed, published, reason, pc, observed, closed>>

(* TLA:CancelReasonPublication.ReadReason *)
ReadReason ==
    /\ readerPC = "read"
    /\ observed' = reason
    /\ readerPC' = "done"
    /\ UNCHANGED <<claimed, published, reason, pc, closed>>

Next ==
    \/ \E c \in Cancellers : Claim(c) \/ WriteReason(c) \/ Publish(c) \/ Close(c)
    \/ Poll
    \/ ReadReason

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ claimed \in BOOLEAN
    /\ published \in BOOLEAN
    /\ closed \in BOOLEAN
    /\ reason \in Cancellers \cup {"none"}
    /\ observed \in Cancellers \cup {"none"}
    /\ pc \in [Cancellers -> {"claim", "write", "publish", "close", "done"}]
    /\ readerPC \in {"poll", "read", "done"}

SingleWriter == Cardinality({c \in Cancellers : pc[c] \in {"write", "publish", "close"}}) <= 1
SafeRead == readerPC \in {"read", "done"} =>
    reason \in Cancellers /\ \A c \in Cancellers : pc[c] # "write"
StableReason == readerPC = "done" => observed = reason
CloseAfterPublication == closed => published /\ reason \in Cancellers

====
