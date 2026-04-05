---- MODULE QuiescenceScope ----
(*******************************************************************************
 * Models the quiescence scope active count and fake_clock hook interaction.
 *
 * The hook should advance time ONLY when all pipeline imps are sleeping
 * (active == 0). The challenge: there's a gap between an imp's leave()
 * (when it sleeps via do_switch) and enter() (when it resumes after being
 * scheduled by a channel match). During this gap, active can momentarily
 * hit 0, causing premature time advancement.
 *
 * Participants:
 *   Imps       — pipeline imps that enter/leave the scope
 *   Hook       — main_loop quiescence hook (fires timers)
 *   Matcher    — alt_end_impl (wakes a sleeping peer via channel match)
 *
 * The spec explores where enter() should happen:
 *   A) At resume time (after do_switch returns) — current implementation
 *   B) At schedule time (in alt_end_impl, before schedule())
 *   C) In push_to_global (before queueing)
 *
 * We model a simple pipeline: Source → Sink, with a timer tick.
 * Source sends values, Sink receives. Tick fires periodically.
 * The hook should only advance when Source, Sink, AND Tick are all sleeping.
 *
 * Expected result: find which enter strategy prevents premature hook firing
 * while preserving liveness (hook fires when all imps are truly done).
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Imps,           \* set of imp IDs, e.g. {src, sink, tick}
    Strategy        \* "resume" | "match" | "push"

VARIABLES
    active,         \* Int: quiescence scope active count
    imp_state,      \* [Imps -> state]: sleeping, queued, running, exited
    hook_fired,     \* Nat: number of times hook has fired
    timer_pending,  \* Bool: a timer is pending (needs advancement)
    enter_strategy  \* "resume" | "match" | "push" — which enter point to use

vars == <<active, imp_state, hook_fired, timer_pending, enter_strategy>>

States == {"sleeping", "queued", "running", "exited"}

(*******************************************************************************
 * HELPERS
 ******************************************************************************)

AllSleepingOrExited ==
    \A i \in Imps : imp_state[i] \in {"sleeping", "exited"}

(*******************************************************************************
 * IMP LIFECYCLE
 ******************************************************************************)

(* Imp spawns: enters scope, starts running. *)
SpawnImp(i) ==
    /\ imp_state[i] = "sleeping"  \* initial state (placeholder)
    /\ FALSE  \* spawning is handled by Init

(* Imp goes to sleep: leave scope. *)
Sleep(i) ==
    /\ imp_state[i] = "running"
    /\ active' = active - 1
    /\ imp_state' = [imp_state EXCEPT ![i] = "sleeping"]
    /\ UNCHANGED <<hook_fired, timer_pending, enter_strategy>>

(* Channel match wakes a sleeping imp: schedule it.
 * Where does enter() happen? Depends on strategy. *)
WakeViaMatch(i) ==
    /\ imp_state[i] = "sleeping"
    /\ imp_state' = [imp_state EXCEPT ![i] = "queued"]
    \* Strategy "match": enter at match time (in alt_end_impl)
    /\ IF enter_strategy = "match"
       THEN active' = active + 1
       ELSE UNCHANGED active
    /\ UNCHANGED <<hook_fired, timer_pending, enter_strategy>>

(* Worker picks up queued imp and runs it. *)
RunQueued(i) ==
    /\ imp_state[i] = "queued"
    /\ imp_state' = [imp_state EXCEPT ![i] = "running"]
    \* Strategy "resume" or "push": enter when running starts
    /\ IF enter_strategy \in {"resume", "push"}
       THEN active' = active + 1
       ELSE UNCHANGED active
    /\ UNCHANGED <<hook_fired, timer_pending, enter_strategy>>

(* Imp exits: leave scope permanently. *)
Exit(i) ==
    /\ imp_state[i] = "running"
    /\ active' = active - 1
    /\ imp_state' = [imp_state EXCEPT ![i] = "exited"]
    /\ UNCHANGED <<hook_fired, timer_pending, enter_strategy>>

(*******************************************************************************
 * HOOK (fake_clock time advancement)
 ******************************************************************************)

(* Hook fires when active == 0. Advances time, which wakes a timer imp.
 * The timer imp transitions from sleeping to queued. *)
HookFire ==
    /\ active = 0
    /\ timer_pending = TRUE
    /\ \E i \in Imps :
        /\ imp_state[i] = "sleeping"
        /\ imp_state' = [imp_state EXCEPT ![i] = "queued"]
    /\ hook_fired' = hook_fired + 1
    /\ timer_pending' = FALSE
    \* Hook fires expired timers which schedule imps. The enter
    \* happens at the same place as any other schedule: depends on strategy.
    /\ IF enter_strategy \in {"push", "match"}
       THEN active' = active + 1
       ELSE UNCHANGED active
    /\ UNCHANGED enter_strategy

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Init ==
    /\ active = Cardinality(Imps)   \* all imps start entered (spawned)
    /\ imp_state = [i \in Imps |-> "running"]
    /\ hook_fired = 0
    /\ timer_pending = TRUE
    /\ enter_strategy = Strategy

Next ==
    \/ \E i \in Imps :
        \/ Sleep(i)
        \/ WakeViaMatch(i)
        \/ RunQueued(i)
        \/ Exit(i)
    \/ HookFire

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ active \in -1..Cardinality(Imps) + 5  \* allow checking for underflow
    /\ imp_state \in [Imps -> States]
    /\ hook_fired \in 0..10
    /\ timer_pending \in BOOLEAN
    /\ enter_strategy \in {"resume", "match", "push"}

(* Safety: active count never goes negative. *)
NonNegativeActive ==
    active >= 0

(* Safety: hook only fires when all imps are truly sleeping or exited.
 * If hook_fired > 0, at the moment it fired, no imp was running or queued. *)
(* This is approximated by: hook can't fire if any imp is running or queued. *)
HookOnlyWhenQuiescent ==
    (active = 0) => ~(\E i \in Imps : imp_state[i] \in {"running", "queued"})

(* Liveness: if a timer is pending and an imp is sleeping (waiting for
 * the timer), the hook eventually fires. *)
HookEventuallyFires ==
    (timer_pending /\ \E i \in Imps : imp_state[i] = "sleeping") ~> (hook_fired > 0 \/ ~timer_pending)

====
