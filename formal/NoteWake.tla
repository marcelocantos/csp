---- MODULE NoteWake ----
(* A single Note::wake races one Note::sleep entry. The older two-CAS
   wake implementation dropped a wake if the sleeper entered SLEEPING
   between the waker's failed sleep CAS and failed flag CAS. *)
EXTENDS Integers

CONSTANT RetrySleepRace
VARIABLES note, sleeper, waker
vars == <<note, sleeper, waker>>

Sleeping == 0
Awake == 1
Flagged == 2

Init == /\ note = Awake
        /\ sleeper = "probe_flag"
        /\ waker = "try_wake"

ConsumeFlag ==
    /\ sleeper = "probe_flag"
    /\ IF note = Flagged
       THEN /\ note' = Awake /\ sleeper' = "done"
       ELSE /\ UNCHANGED note /\ sleeper' = "try_sleep"
    /\ UNCHANGED waker

TrySleep ==
    /\ sleeper = "try_sleep"
    /\ IF note = Awake
       THEN /\ note' = Sleeping /\ sleeper' = "wait"
       ELSE /\ note' = Awake /\ sleeper' = "done"
    /\ UNCHANGED waker

ObserveWake ==
    /\ sleeper = "wait" /\ note # Sleeping
    /\ note' = Awake /\ sleeper' = "done"
    /\ UNCHANGED waker

(* TLA:NoteWake.TryWake *)
TryWake ==
    /\ waker = "try_wake"
    /\ IF note = Sleeping
       THEN /\ note' = Awake /\ waker' = "done"
       ELSE /\ UNCHANGED note /\ waker' = "try_flag"
    /\ UNCHANGED sleeper

(* TLA:NoteWake.TryFlag *)
TryFlag ==
    /\ waker = "try_flag"
    /\ IF note = Awake
       THEN /\ note' = Flagged /\ waker' = "done"
       ELSE /\ UNCHANGED note
            /\ waker' = IF note = Sleeping /\ RetrySleepRace
                          THEN "try_wake" ELSE "done"
    /\ UNCHANGED sleeper

Next == ConsumeFlag \/ TrySleep \/ ObserveWake \/ TryWake \/ TryFlag
Spec == Init /\ [][Next]_vars

TypeOK == /\ note \in {Sleeping, Awake, Flagged}
          /\ sleeper \in {"probe_flag", "try_sleep", "wait", "done"}
          /\ waker \in {"try_wake", "try_flag", "done"}
NoLostWake == (waker = "done" /\ sleeper = "wait") => note # Sleeping
====
