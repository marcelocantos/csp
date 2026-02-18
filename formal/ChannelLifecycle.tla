---- MODULE ChannelLifecycle ----
(*******************************************************************************
 * Models the channel endpoint release and waiter wakeup protocol from
 * csp/src/channel.cc.
 *
 * The protocol ensures:
 *   - No use-after-free: channel state is never accessed after deletion.
 *   - No lost waiter: every WAITING waiter is scheduled before the channel
 *     is freed.
 *   - Exactly one delete: alive_ reaches 0 on exactly one thread.
 *
 * Participants:
 *   CloserW  — thread that closes the writer endpoint (refcount → 0)
 *   CloserR  — thread that closes the reader endpoint (refcount → 0)
 *   Waiter   — microthread in prialt_begin_impl, holding a reader endpoint
 *
 * The waiter holds a reader reference. It enters prialt_begin_impl (which
 * uses the channel), then returns, and only then releases the reader
 * endpoint. This means CloserR (last reader release) cannot begin until
 * after the waiter finishes with the channel. CloserW can run concurrently
 * since the writer side is independent.
 *
 * We model CloserW as the "first closer" that can run concurrently with
 * the waiter, and CloserR as the "second closer" that runs after the
 * waiter releases its reference.
 *
 * Code references (src/channel.cc, include/csp/internal/csp_internal.h):
 *   CloserDecRef     — channel.cc:122: fetch_sub refcount; if 1, enter release
 *   CloserAcquire    — channel.cc:124: lock mu_
 *   CloserWakeWaiter — channel.cc:130-136: CAS WAITING→CLAIMED, schedule()
 *   CloserReleaseMu  — channel.cc:149: unlock mu_ (end of lock_guard scope)
 *   CloserDecAlive   — channel.cc:154: fetch_sub(alive_); if 1, delete
 *   WaiterAcquire    — channel.cc:228: lock_all()
 *   WaiterPhase1     — channel.cc:237-293: scan for ready peer or dead
 *   RegisterWaiter   — channel.cc:301-308: set alt_state=WAITING, add to waiters
 *   WaiterSleep      — channel.cc:319-321: suspending=true, unlock, switch
 *   WaiterWake       — channel.cc:325-333: re-lock, clean up registrations
 *   WaiterDone       — channel.cc:335: set alt_state=IDLE
 ******************************************************************************)

EXTENDS Integers, FiniteSets

VARIABLES
    endpt_dead,     \* Function: {"W","R"} -> Boolean (endpoint refcount hit zero)
    alive,          \* Integer 0..2: channel alive counter
    channel_live,   \* Boolean: channel object has not been deleted
    ch_mu,          \* "none" | id: who holds channel mutex
    waiter_set,     \* Set of waiters registered on the channel
    alt_state,      \* "IDLE" | "WAITING" | "CLAIMED" (single waiter)
    on_queue,       \* Boolean: waiter is on run queue
    pc_cw,          \* CloserW program counter
    pc_cr,          \* CloserR program counter
    pc_waiter       \* Waiter program counter

Closers == {"W", "R"}

vars == <<endpt_dead, alive, channel_live, ch_mu, waiter_set,
          alt_state, on_queue, pc_cw, pc_cr, pc_waiter>>

(*******************************************************************************
 * Initial state: channel alive with both endpoints open, waiter idle.
 ******************************************************************************)
Init ==
    /\ endpt_dead = [c \in Closers |-> FALSE]
    /\ alive = 2
    /\ channel_live = TRUE
    /\ ch_mu = "none"
    /\ waiter_set = {}
    /\ alt_state = "IDLE"
    /\ on_queue = TRUE
    /\ pc_cw = "start"
    /\ pc_cr = "idle"   \* CloserR waits until waiter releases its ref
    /\ pc_waiter = "start"

(*******************************************************************************
 * WAITER ACTIONS
 *
 * Models prialt_begin_impl: phase 1 (scan for ready/dead), phase 2
 * (register + sleep), and phase 3 (wake + cleanup). After cleanup, the
 * waiter releases its reader reference, enabling CloserR.
 ******************************************************************************)

(* Waiter acquires channel mutex.
 * Code: channel.cc:228 lock_all()
 *
 * TLA:ChannelLifecycle.WaiterAcquire *)
WaiterAcquire ==
    /\ pc_waiter = "start"
    /\ channel_live
    /\ ch_mu = "none"
    /\ ch_mu' = "wa"
    /\ pc_waiter' = "phase1"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cw, pc_cr>>

(* Phase 1: scan channel under lock. If writer endpoint is dead (CloserW
 * has decremented its refcount), the channel is dead — return with dead
 * result, unlock, and release the waiter's reader ref (enabling CloserR).
 *
 * If channel is alive, proceed to phase 2 (register).
 *
 * Code: channel.cc:244 if (!*ch) { ... unlock_all(); return; }
 *
 * TLA:ChannelLifecycle.WaiterPhase1 *)
WaiterPhase1 ==
    /\ pc_waiter = "phase1"
    /\ IF endpt_dead["W"]
       THEN \* Channel dead: unlock, return, release reader ref
            /\ ch_mu' = "none"
            /\ pc_waiter' = "release_ref"
            /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                           alt_state, on_queue, pc_cw, pc_cr>>
       ELSE \* Channel alive: proceed to registration
            /\ pc_waiter' = "registering"
            /\ UNCHANGED <<endpt_dead, alive, channel_live, ch_mu,
                           waiter_set, alt_state, on_queue, pc_cw, pc_cr>>

(* Phase 2: set alt_state=WAITING, add to waiters under lock.
 * Single action because both happen under mu_ before unlock.
 *
 * Code: channel.cc:301-308
 *   g_self->alt_state.store(ALT_WAITING, release);
 *   ch->endpts_[endpt].wait(&chop);
 *
 * TLA:ChannelLifecycle.RegisterWaiter *)
RegisterWaiter ==
    /\ pc_waiter = "registering"
    /\ alt_state' = "WAITING"
    /\ waiter_set' = waiter_set \union {"wa"}
    /\ pc_waiter' = "sleeping"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, ch_mu,
                   on_queue, pc_cw, pc_cr>>

(* Waiter releases lock and context-switches out.
 * Single action: unlock + switch happen together from waiter's perspective.
 *
 * Code: channel.cc:319-321
 *   g_self->suspending_.store(true, release);
 *   unlock_all();
 *   do_switch(Status::detach);
 *
 * TLA:ChannelLifecycle.WaiterSleep *)
WaiterSleep ==
    /\ pc_waiter = "sleeping"
    /\ ch_mu' = "none"
    /\ on_queue' = FALSE
    /\ pc_waiter' = "asleep"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, pc_cw, pc_cr>>

(* Waiter wakes up (scheduled by closer). Re-acquires lock for cleanup.
 * Enabled only after being claimed (on_queue) and channel still exists.
 *
 * Code: channel.cc:325 lock_all()
 *
 * TLA:ChannelLifecycle.WaiterWakeAcquire *)
WaiterWakeAcquire ==
    /\ pc_waiter = "asleep"
    /\ on_queue
    /\ channel_live
    /\ ch_mu = "none"
    /\ ch_mu' = "wa"
    /\ pc_waiter' = "cleaning"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cw, pc_cr>>

(* Waiter cleans up: removes from waiters, sets alt_state=IDLE, unlocks.
 * Single action: all under mu_.
 *
 * Code: channel.cc:326-335
 *   ch->endpts_[endpt].remove(&chop, g_self);
 *   unlock_all();
 *   g_self->alt_state.store(ALT_IDLE, release);
 *
 * TLA:ChannelLifecycle.WaiterCleanup *)
WaiterCleanup ==
    /\ pc_waiter = "cleaning"
    /\ waiter_set' = waiter_set \ {"wa"}
    /\ alt_state' = "IDLE"
    /\ ch_mu' = "none"
    /\ pc_waiter' = "release_ref"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, on_queue,
                   pc_cw, pc_cr>>

(* Waiter releases its reader endpoint reference. This enables CloserR.
 * Models the chan_op destructor calling reader_release after
 * prialt_begin_impl returns.
 *
 * Code: csp.h chan_op<T>::~chan_op() → reader_release / writer_release
 *
 * TLA:ChannelLifecycle.WaiterReleaseRef *)
WaiterReleaseRef ==
    /\ pc_waiter = "release_ref"
    /\ pc_waiter' = "done_w"
    /\ pc_cr' = "start"     \* Enable CloserR now that waiter's ref is gone
    /\ UNCHANGED <<endpt_dead, alive, channel_live, ch_mu, waiter_set,
                   alt_state, on_queue, pc_cw>>

(*******************************************************************************
 * CLOSER W ACTIONS
 *
 * Writer closer: runs independently, can race with the waiter. When it
 * enters release(), it marks the writer endpoint dead, acquires mu_,
 * wakes any registered waiters via CAS, releases mu_, then decrements
 * alive_.
 ******************************************************************************)

(* CloserW decrements writer refcount to zero (atomic).
 * Code: channel.cc:122
 *
 * TLA:ChannelLifecycle.CloserWDecRef *)
CloserWDecRef ==
    /\ pc_cw = "start"
    /\ channel_live
    /\ endpt_dead' = [endpt_dead EXCEPT !["W"] = TRUE]
    /\ pc_cw' = "want_mu"
    /\ UNCHANGED <<alive, channel_live, ch_mu, waiter_set,
                   alt_state, on_queue, pc_cr, pc_waiter>>

(* CloserW acquires mu_.
 * Code: channel.cc:124
 *
 * TLA:ChannelLifecycle.CloserWAcquire *)
CloserWAcquire ==
    /\ pc_cw = "want_mu"
    /\ ch_mu = "none"
    /\ ch_mu' = "W"
    /\ pc_cw' = "waking"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cr, pc_waiter>>

(* CloserW wakes waiters under mu_ via CAS WAITING→CLAIMED.
 * Code: channel.cc:130-146
 *
 * TLA:ChannelLifecycle.CloserWWakeWaiters *)
CloserWWakeWaiters ==
    /\ pc_cw = "waking"
    /\ IF "wa" \in waiter_set /\ alt_state = "WAITING"
       THEN /\ alt_state' = "CLAIMED"
            /\ on_queue' = TRUE
       ELSE UNCHANGED <<alt_state, on_queue>>
    /\ pc_cw' = "unlock_mu"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, ch_mu,
                   waiter_set, pc_cr, pc_waiter>>

(* CloserW releases mu_.
 * Code: channel.cc:149
 *
 * TLA:ChannelLifecycle.CloserWReleaseMu *)
CloserWReleaseMu ==
    /\ pc_cw = "unlock_mu"
    /\ ch_mu' = "none"
    /\ pc_cw' = "dec_alive"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cr, pc_waiter>>

(* CloserW decrements alive_. If it was 1, delete channel.
 * Code: channel.cc:154-156
 *
 * TLA:ChannelLifecycle.CloserWDecAlive *)
CloserWDecAlive ==
    /\ pc_cw = "dec_alive"
    /\ channel_live
    /\ alive' = alive - 1
    /\ channel_live' = (alive - 1 /= 0)
    /\ pc_cw' = "done_cw"
    /\ UNCHANGED <<endpt_dead, ch_mu, waiter_set, alt_state,
                   on_queue, pc_cr, pc_waiter>>

(*******************************************************************************
 * CLOSER R ACTIONS
 *
 * Reader closer: can only start after the waiter releases its reader
 * reference (pc_cr starts at "idle", moves to "start" when waiter
 * releases ref). Same protocol as CloserW but for the reader side.
 ******************************************************************************)

(* CloserR decrements reader refcount to zero.
 * Code: channel.cc:122
 *
 * TLA:ChannelLifecycle.CloserRDecRef *)
CloserRDecRef ==
    /\ pc_cr = "start"
    /\ channel_live
    /\ endpt_dead' = [endpt_dead EXCEPT !["R"] = TRUE]
    /\ pc_cr' = "want_mu"
    /\ UNCHANGED <<alive, channel_live, ch_mu, waiter_set,
                   alt_state, on_queue, pc_cw, pc_waiter>>

(* CloserR acquires mu_.
 * Code: channel.cc:124
 *
 * TLA:ChannelLifecycle.CloserRAcquire *)
CloserRAcquire ==
    /\ pc_cr = "want_mu"
    /\ ch_mu = "none"
    /\ ch_mu' = "R"
    /\ pc_cr' = "waking"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cw, pc_waiter>>

(* CloserR wakes waiters under mu_ (but waiter has already cleaned up
 * by this point, so normally no-one to wake).
 * Code: channel.cc:130-146
 *
 * TLA:ChannelLifecycle.CloserRWakeWaiters *)
CloserRWakeWaiters ==
    /\ pc_cr = "waking"
    /\ IF "wa" \in waiter_set /\ alt_state = "WAITING"
       THEN /\ alt_state' = "CLAIMED"
            /\ on_queue' = TRUE
       ELSE UNCHANGED <<alt_state, on_queue>>
    /\ pc_cr' = "unlock_mu"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, ch_mu,
                   waiter_set, pc_cw, pc_waiter>>

(* CloserR releases mu_.
 * Code: channel.cc:149
 *
 * TLA:ChannelLifecycle.CloserRReleaseMu *)
CloserRReleaseMu ==
    /\ pc_cr = "unlock_mu"
    /\ ch_mu' = "none"
    /\ pc_cr' = "dec_alive"
    /\ UNCHANGED <<endpt_dead, alive, channel_live, waiter_set,
                   alt_state, on_queue, pc_cw, pc_waiter>>

(* CloserR decrements alive_. If it was 1, delete channel.
 * Code: channel.cc:154-156
 *
 * TLA:ChannelLifecycle.CloserRDecAlive *)
CloserRDecAlive ==
    /\ pc_cr = "dec_alive"
    /\ channel_live
    /\ alive' = alive - 1
    /\ channel_live' = (alive - 1 /= 0)
    /\ pc_cr' = "done_cr"
    /\ UNCHANGED <<endpt_dead, ch_mu, waiter_set, alt_state,
                   on_queue, pc_cw, pc_waiter>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WaiterAcquire
    \/ WaiterPhase1
    \/ RegisterWaiter
    \/ WaiterSleep
    \/ WaiterWakeAcquire
    \/ WaiterCleanup
    \/ WaiterReleaseRef
    \/ CloserWDecRef
    \/ CloserWAcquire
    \/ CloserWWakeWaiters
    \/ CloserWReleaseMu
    \/ CloserWDecAlive
    \/ CloserRDecRef
    \/ CloserRAcquire
    \/ CloserRWakeWaiters
    \/ CloserRReleaseMu
    \/ CloserRDecAlive

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant *)
TypeOK ==
    /\ \A c \in Closers : endpt_dead[c] \in BOOLEAN
    /\ alive \in 0..2
    /\ channel_live \in BOOLEAN
    /\ ch_mu \in {"none", "W", "R", "wa"}
    /\ waiter_set \subseteq {"wa"}
    /\ alt_state \in {"IDLE", "WAITING", "CLAIMED"}
    /\ on_queue \in BOOLEAN
    /\ pc_cw \in {"start", "want_mu", "waking", "unlock_mu",
                   "dec_alive", "done_cw"}
    /\ pc_cr \in {"idle", "start", "want_mu", "waking", "unlock_mu",
                   "dec_alive", "done_cr"}
    /\ pc_waiter \in {"start", "phase1", "registering", "sleeping",
                       "asleep", "cleaning", "release_ref", "done_w"}

(* Safety: no thread accesses channel state after deletion. Any thread
 * in a state that will access channel state must have channel_live. *)
NoUseAfterFree ==
    /\ pc_cw \in {"want_mu", "waking", "unlock_mu", "dec_alive"}
        => channel_live
    /\ pc_cr \in {"want_mu", "waking", "unlock_mu", "dec_alive"}
        => channel_live
    /\ pc_waiter \in {"phase1", "registering", "sleeping", "cleaning"}
        => channel_live

(* Safety: once both closers are done, a waiter still in WAITING state
 * must be on a queue. *)
NoLostWaiter ==
    (pc_cw = "done_cw" /\ pc_cr = "done_cr") =>
        (alt_state = "WAITING" => on_queue)

(* Safety: alive never goes negative. *)
AliveNonNegative ==
    alive >= 0

(* Safety: if the waiter is CLAIMED, it is on queue or already cleaning. *)
ClaimedImpliesScheduled ==
    (alt_state = "CLAIMED") =>
        (on_queue \/ pc_waiter \in {"cleaning", "release_ref", "done_w"})

====
