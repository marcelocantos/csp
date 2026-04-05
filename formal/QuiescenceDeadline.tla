---- MODULE QuiescenceDeadline ----
(*******************************************************************************
 * Models the specific "deadline fires timed_out" scenario:
 *
 * csp::run imp (R):
 *   - binds clock, creates cancel scope with 5s deadline
 *   - spawns child C and deadline timer T
 *   - waits on prialt(done())
 *
 * Deadline timer imp (T):
 *   - sleeps on fake timer (5s) via prialt(~cancel_signal, timer)
 *   - when timer fires: signals cancel, exits
 *   - when cancel_signal fires first: exits
 *
 * Child imp (C):
 *   - sleeps on fake timer (60s) via prialt(~cancel_signal, timer)
 *   - when cancel fires: catches timed_out, exits
 *   - when timer fires: exits normally
 *
 * Hook:
 *   - fires when active == 0
 *   - advances to next deadline (5s first, then 60s)
 *
 * The question: with "match" enter strategy, does T entering the scope
 * after hook fires prevent the hook from firing again for C?
 *
 * Answer: NO, because T runs, fires the cancel, C wakes via channel
 * match (enter), T exits (leave), C runs, exits (leave). active drops
 * to 0. Hook fires for the 60s timer but nobody is sleeping → returns
 * false → done.
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    active,     \* Int: quiescence scope active count
    r_state,    \* "running" | "waiting" | "exited"
    t_state,    \* "running" | "sleeping" | "queued" | "handling" | "exited"
    c_state,    \* "running" | "sleeping" | "queued" | "handling" | "exited"
    hook_count, \* Nat: times hook fired
    t5_pending, \* Bool: 5s deadline timer pending
    t60_pending,\* Bool: 60s child timer pending
    cancel_fired \* Bool: cancel signal sent

vars == <<active, r_state, t_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

Init ==
    /\ active = 3       \* R, T, C all spawned and entered
    /\ r_state = "running"
    /\ t_state = "running"
    /\ c_state = "running"
    /\ hook_count = 0
    /\ t5_pending = TRUE
    /\ t60_pending = TRUE
    /\ cancel_fired = FALSE

(*******************************************************************************
 * R: csp::run imp
 ******************************************************************************)

(* R enters prialt(done()) — sleeps waiting for cancel *)
RSleep ==
    /\ r_state = "running"
    /\ r_state' = "waiting"
    /\ active' = active - 1
    /\ UNCHANGED <<t_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* R wakes when cancel fires (done() triggers — channel match enters,
 * then exits immediately: enter + leave = net 0) *)
RWake ==
    /\ r_state = "waiting"
    /\ cancel_fired = TRUE
    /\ r_state' = "exited"
    /\ active' = active   \* +1 enter (match), -1 leave (exit) = net 0
    /\ UNCHANGED <<t_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(*******************************************************************************
 * T: deadline timer imp
 ******************************************************************************)

(* T enters prialt(~cancel_signal, timer) — sleeps *)
TSleep ==
    /\ t_state = "running"
    /\ t_state' = "sleeping"
    /\ active' = active - 1
    /\ UNCHANGED <<r_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* T wakes because 5s timer fired (via hook) — queued *)
TWakeTimer ==
    /\ t_state = "sleeping"
    /\ t5_pending = FALSE  \* hook already fired and cleared this
    /\ t_state' = "queued"
    /\ UNCHANGED <<active, r_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* T runs after being queued (already entered at match/fire time) *)
TRun ==
    /\ t_state = "queued"
    /\ t_state' = "handling"
    /\ UNCHANGED <<active, r_state, c_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* T fires cancel and exits *)
TFireCancel ==
    /\ t_state = "handling"
    /\ cancel_fired' = TRUE
    /\ t_state' = "exited"
    /\ active' = active - 1
    /\ UNCHANGED <<r_state, c_state, hook_count, t5_pending, t60_pending>>

(*******************************************************************************
 * C: child imp
 ******************************************************************************)

(* C enters prialt(~cancel_signal, timer) — sleeps *)
CSleep ==
    /\ c_state = "running"
    /\ c_state' = "sleeping"
    /\ active' = active - 1
    /\ UNCHANGED <<r_state, t_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* C wakes because cancel fired (channel match — enter at match time) *)
CWakeCancel ==
    /\ c_state = "sleeping"
    /\ cancel_fired = TRUE
    /\ c_state' = "queued"
    /\ active' = active + 1   \* enter at match time (alt_end_impl)
    /\ UNCHANGED <<r_state, t_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* C runs after being queued (already entered at match/fire time) *)
CRun ==
    /\ c_state = "queued"
    /\ c_state' = "handling"
    /\ UNCHANGED <<active, r_state, t_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(* C catches timed_out and exits *)
CExit ==
    /\ c_state = "handling"
    /\ c_state' = "exited"
    /\ active' = active - 1
    /\ UNCHANGED <<r_state, t_state, hook_count, t5_pending, t60_pending, cancel_fired>>

(*******************************************************************************
 * HOOK
 ******************************************************************************)

(* Hook fires when active == 0 and a timer is pending *)
HookFire5 ==
    /\ active = 0
    /\ t5_pending = TRUE
    /\ t5_pending' = FALSE
    /\ t_state = "sleeping"
    /\ t_state' = "queued"
    /\ active' = active + 1   \* enter at fire time (inside hook, before schedule)
    /\ hook_count' = hook_count + 1
    /\ UNCHANGED <<r_state, c_state, t60_pending, cancel_fired>>

HookFire60 ==
    /\ active = 0
    /\ t60_pending = TRUE
    /\ t5_pending = FALSE
    /\ c_state = "sleeping"
    /\ c_state' = "queued"
    /\ active' = active + 1   \* enter at fire time
    /\ t60_pending' = FALSE
    /\ hook_count' = hook_count + 1
    /\ UNCHANGED <<r_state, t_state, t5_pending, cancel_fired>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ RSleep \/ RWake
    \/ TSleep \/ TWakeTimer \/ TRun \/ TFireCancel
    \/ CSleep \/ CWakeCancel \/ CRun \/ CExit
    \/ HookFire5 \/ HookFire60

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ active \in -1..5
    /\ r_state \in {"running", "waiting", "exited"}
    /\ t_state \in {"running", "sleeping", "queued", "handling", "exited"}
    /\ c_state \in {"running", "sleeping", "queued", "handling", "exited"}
    /\ hook_count \in 0..5
    /\ t5_pending \in BOOLEAN
    /\ t60_pending \in BOOLEAN
    /\ cancel_fired \in BOOLEAN

NonNegativeActive == active >= 0

(* Safety: hook only fires when no imp is running, queued, or handling *)
HookSafe ==
    (active = 0) =>
        /\ r_state \in {"waiting", "exited"}
        /\ t_state \in {"sleeping", "exited"}
        /\ c_state \in {"sleeping", "exited"}

(* Liveness: all imps eventually exit *)
AllExit == <>(r_state = "exited" /\ t_state = "exited" /\ c_state = "exited")

====
