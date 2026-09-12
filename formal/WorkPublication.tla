---- MODULE WorkPublication ----
(* One task publisher races one idle worker's registration and final work
   check. The Note's wake/sleep transition is atomic here: NoteWake models
   its internal CAS race separately.

   A release store need not yet be visible to an acquire load that reads the
   old value. workVisible models that delayed publication. The worker always
   executes an SC fence after registering parked (notify_quiesce_watchers).
   PublisherFence flushes the publisher's store before either wake scan.

   This is a bounded store-buffer abstraction, not a general C++ memory-model
   checker. The C++ SC-order argument is: if both final reads miss the other's
   store, parked coherence requires publisherFence < workerFence, while work
   coherence requires workerFence < publisherFence. Both cannot hold in the
   SC total order. Without publisherFence no such contradiction exists.
   See https://eel.is/c++draft/atomics.order, paragraph 4.

   Scope: one publisher, one worker, one queue insertion, one park cycle;
   no queue draining, worker retirement, scheduler fairness or Note internals.
   NoStrandedWork is safety at the end of the wake scan, not a liveness claim.
*)
EXTENDS TLC

CONSTANT FencePublisher
VARIABLES queued, workVisible, parked, wake, note, publisher, worker
vars == <<queued, workVisible, parked, wake, note, publisher, worker>>

Init == /\ queued = FALSE /\ workVisible = FALSE /\ parked = FALSE
        /\ wake = FALSE /\ note = "awake"
        /\ publisher = "enqueue" /\ worker = "register"

Enqueue ==
    /\ publisher = "enqueue"
    /\ queued' = TRUE /\ publisher' = "fence"
    /\ UNCHANGED <<workVisible, parked, wake, note, worker>>

PublishStore ==
    /\ queued /\ ~workVisible
    /\ workVisible' = TRUE
    /\ UNCHANGED <<queued, parked, wake, note, publisher, worker>>

PublisherFence ==
    /\ publisher = "fence"
    /\ workVisible' = IF FencePublisher THEN TRUE ELSE workVisible
    /\ publisher' = "scan_note"
    /\ UNCHANGED <<queued, parked, wake, note, worker>>

ScanNote ==
    /\ publisher = "scan_note"
    /\ IF note = "sleeping"
       THEN /\ wake' = TRUE /\ publisher' = "done"
       ELSE /\ UNCHANGED wake /\ publisher' = "scan_parked"
    /\ UNCHANGED <<queued, workVisible, parked, note, worker>>

ScanParked ==
    /\ publisher = "scan_parked"
    /\ wake' = (wake \/ parked)
    /\ publisher' = "done"
    /\ UNCHANGED <<queued, workVisible, parked, note, worker>>

Register ==
    /\ worker = "register"
    /\ worker' = "fence"
    /\ UNCHANGED <<queued, workVisible, parked, wake, note, publisher>>

WorkerFence ==
    /\ worker = "fence"
    /\ parked' = TRUE
    /\ worker' = "check_work"
    /\ UNCHANGED <<queued, workVisible, wake, note, publisher>>

CheckWork ==
    /\ worker = "check_work"
    /\ worker' = IF workVisible THEN "working" ELSE "try_sleep"
    /\ UNCHANGED <<queued, workVisible, parked, wake, note, publisher>>

TrySleep ==
    /\ worker = "try_sleep"
    /\ worker' = IF wake THEN "working" ELSE "sleeping"
    /\ note' = IF wake THEN "awake" ELSE "sleeping"
    /\ UNCHANGED <<queued, workVisible, parked, wake, publisher>>

ObserveWake ==
    /\ worker = "sleeping" /\ wake
    /\ worker' = "working" /\ note' = "awake"
    /\ UNCHANGED <<queued, workVisible, parked, wake, publisher>>

Next == Enqueue \/ PublishStore \/ PublisherFence \/ ScanNote \/ ScanParked
        \/ Register \/ WorkerFence \/ CheckWork \/ TrySleep \/ ObserveWake
Spec == Init /\ [][Next]_vars

TypeOK == /\ queued \in BOOLEAN /\ workVisible \in BOOLEAN
          /\ parked \in BOOLEAN /\ wake \in BOOLEAN
          /\ note \in {"awake", "sleeping"}
          /\ publisher \in {"enqueue", "fence", "scan_note", "scan_parked", "done"}
          /\ worker \in {"register", "fence", "check_work", "try_sleep", "sleeping", "working"}
NoStrandedWork ==
    (queued /\ publisher = "done" /\ worker = "sleeping") => wake
====
