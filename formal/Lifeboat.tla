----------------------------- MODULE Lifeboat -----------------------------
EXTENDS Naturals, FiniteSets, TLC
CONSTANT Cargo, Capacity
VARIABLES owner, admitted, delivered, accepting
vars == <<owner, admitted, delivered, accepting>>
Stages == {"approach", "craneA", "craneB", "warehouse", "factory", "tram"}
At(s) == {c \in DOMAIN owner : owner[c] = s}
Init == /\ owner = [c \in {} |-> "approach"]
        /\ admitted = {} /\ delivered = {} /\ accepting = TRUE
Admit(c) == /\ accepting /\ c \notin admitted
            /\ Cardinality(At("approach")) < Capacity
            /\ owner' = owner @@ (c :> "approach")
            /\ admitted' = admitted \cup {c}
            /\ UNCHANGED <<delivered, accepting>>
Move(c, a, b) == /\ c \in At(a) /\ Cardinality(At(b)) < Capacity
                 /\ owner' = [owner EXCEPT ![c] = b]
                 /\ UNCHANGED <<admitted, delivered, accepting>>
Deliver(c) == /\ c \in At("tram")
              /\ owner' = [x \in DOMAIN owner \ {c} |-> owner[x]]
              /\ delivered' = delivered \cup {c}
              /\ UNCHANGED <<admitted, accepting>>
Evacuate == /\ accepting /\ accepting' = FALSE
            /\ UNCHANGED <<owner, admitted, delivered>>
Route == \/ \E c \in Cargo, crane \in {"craneA", "craneB"} :
             Move(c, "approach", crane) \/ Move(c, crane, "warehouse")
        \/ \E c \in Cargo : Move(c, "warehouse", "factory")
                         \/ Move(c, "factory", "tram") \/ Deliver(c)
Next == (\E c \in Cargo : Admit(c)) \/ Route \/ Evacuate
Conservation == /\ admitted = DOMAIN owner \cup delivered
                /\ DOMAIN owner \cap delivered = {}
Bounded == \A s \in Stages : Cardinality(At(s)) <= Capacity
Spec == Init /\ [][Next]_vars /\ WF_vars(Route)
Drain == (~accepting) ~> (DOMAIN owner = {})
=============================================================================
