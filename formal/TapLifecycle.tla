---- MODULE TapLifecycle ----
(*******************************************************************************
 * Models the tap forwarder lifecycle: splice, forward, auto-fuse-back.
 *
 * tap(w, r) splices a forwarder imp between writer w and reader r,
 * creating two interposed channels (A and B) and a tap observation
 * channel.  The forwarder reads from B (upstream), writes to A
 * (downstream) and to the tap channel.  When the tap_handle is
 * destroyed, it kills the tap reader and fuses w↔r back, which kills
 * the B and A endpoints visible to the forwarder, causing it to exit.
 *
 * Participants:
 *   Producer  — implicit; sends MaxVals values via writer w → channel B.
 *   Forwarder — imp that reads B, writes tap_ch and A in a loop.
 *   Handle    — ~tap_handle: kills tap output, then fuses.
 *   Consumer  — implicit; reads values from channel A → reader r.
 *
 * The protocol ensures:
 *   - At-most-one-lost: when forwarder exits, produced - consumed ≤ 1.
 *   - Fuse kills channels: handle destruction reliably kills B and A
 *     endpoints, so the forwarder can detect death and exit.
 *   - No reference cycle: w_copy_ and r_copy_ live in the handle
 *     (not the forwarder), so fuse can kill intermediate channels.
 *
 * Code references (include/csp/csp.h):
 *   tap()          — csp.h:644-668
 *   tap_handle     — csp.h:620-642
 *   ~tap_handle    — csp.h:630-633
 *   fuse()         — csp.h:603-608
 *   Forwarder loop — csp.h:655-661
 ******************************************************************************)

EXTENDS Integers

CONSTANTS MaxVals   \* Small value count (2) for tractable state space

VARIABLES
    produced,       \* 0..MaxVals: values sent by producer to B
    consumed,       \* 0..MaxVals: values forwarded to consumer via A
    tapped,         \* 0..MaxVals: values copied to tap channel
    fwd_buf,        \* 0..1: value in flight in forwarder
    b_dead,         \* BOOLEAN: B's writer endpoint dead (no upstream data)
    a_dead,         \* BOOLEAN: A's reader endpoint dead (no downstream)
    tap_dead,       \* BOOLEAN: tap reader endpoint dead
    fwd_tw,         \* BOOLEAN: forwarder's tap writer (tw) still alive
    fused,          \* BOOLEAN: fuse-back completed
    pc_fwd,         \* forwarder program counter
    pc_handle       \* handle program counter

vars == <<produced, consumed, tapped, fwd_buf, b_dead, a_dead,
          tap_dead, fwd_tw, fused, pc_fwd, pc_handle>>

(*******************************************************************************
 * Initial state: tap has been called, forwarder is spawned and running,
 * handle is alive.  Channels A, B, and tap_ch all have live endpoints.
 ******************************************************************************)
Init ==
    /\ produced  = 0
    /\ consumed  = 0
    /\ tapped    = 0
    /\ fwd_buf   = 0
    /\ b_dead    = FALSE
    /\ a_dead    = FALSE
    /\ tap_dead  = FALSE
    /\ fwd_tw    = TRUE
    /\ fused     = FALSE
    /\ pc_fwd    = "read_wait"
    /\ pc_handle = "alive"

(*******************************************************************************
 * FORWARDER + PRODUCER/CONSUMER ACTIONS
 *
 * The forwarder loop:
 *   for (T t; prialt(~aw, br >> t) >= 0;) {
 *       if (tw && !(tw << t)) tw = {};
 *       if (!(aw << t)) break;
 *   }
 *
 * Since channels are synchronous, producer-send and consumer-receive
 * are modeled as rendezvous actions with the forwarder.
 ******************************************************************************)

(* Synchronous rendezvous: producer writes to B, forwarder reads.
 * Models: prialt(~aw, br >> t) succeeding with br >> t match.
 *
 * Code: csp.h:655 — prialt(~aw, br >> t)
 *
 * TLA:TapLifecycle.ProducerSend *)
ProducerSend ==
    /\ pc_fwd = "read_wait"
    /\ produced < MaxVals
    /\ ~b_dead
    /\ ~a_dead                  \* ~aw death watch would fire if a_dead
    /\ produced' = produced + 1
    /\ fwd_buf' = 1
    /\ pc_fwd' = "tap_try"
    /\ UNCHANGED <<consumed, tapped, b_dead, a_dead, tap_dead,
                   fwd_tw, fused, pc_handle>>

(* Forwarder detects upstream or downstream death at prialt.
 * ~aw fires if A's reader died; br >> t fails if B's writer died.
 * Either way, prialt returns < 0, loop exits.
 *
 * Code: csp.h:655 — prialt returns < 0
 *
 * TLA:TapLifecycle.ForwarderReadDeath *)
ForwarderReadDeath ==
    /\ pc_fwd = "read_wait"
    /\ (b_dead \/ a_dead)
    /\ pc_fwd' = "exited"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fwd_tw, fused, pc_handle>>

(* Tap write succeeds: value copied to tap channel.
 *
 * Code: csp.h:657 — if (tw && !(tw << t)) tw = {};
 *       [tw << t succeeds]
 *
 * TLA:TapLifecycle.ForwarderTap *)
ForwarderTap ==
    /\ pc_fwd = "tap_try"
    /\ fwd_tw
    /\ ~tap_dead
    /\ tapped' = tapped + 1
    /\ pc_fwd' = "write_wait"
    /\ UNCHANGED <<produced, consumed, fwd_buf, b_dead, a_dead,
                   tap_dead, fwd_tw, fused, pc_handle>>

(* Tap write skipped: tap reader dead or tw already cleared.
 *
 * Code: csp.h:657 — if (tw && !(tw << t)) tw = {};
 *       [tw is false, or tw << t fails → tw = {}]
 *
 * TLA:TapLifecycle.ForwarderSkipTap *)
ForwarderSkipTap ==
    /\ pc_fwd = "tap_try"
    /\ (tap_dead \/ ~fwd_tw)
    /\ fwd_tw' = FALSE
    /\ pc_fwd' = "write_wait"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fused, pc_handle>>

(* Synchronous rendezvous: forwarder writes to A, consumer reads.
 *
 * Code: csp.h:658 — if (!(aw << t)) break;
 *       [aw << t succeeds]
 *
 * TLA:TapLifecycle.ForwarderWrite *)
ForwarderWrite ==
    /\ pc_fwd = "write_wait"
    /\ ~a_dead
    /\ consumed' = consumed + 1
    /\ fwd_buf' = 0
    /\ pc_fwd' = "read_wait"
    /\ UNCHANGED <<produced, tapped, b_dead, a_dead, tap_dead,
                   fwd_tw, fused, pc_handle>>

(* Forwarder's write to A fails: downstream dead.
 *
 * Code: csp.h:658 — if (!(aw << t)) break;
 *       [aw << t fails → break]
 *
 * TLA:TapLifecycle.ForwarderWriteFail *)
ForwarderWriteFail ==
    /\ pc_fwd = "write_wait"
    /\ a_dead
    /\ pc_fwd' = "exited"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fwd_tw, fused, pc_handle>>

(*******************************************************************************
 * HANDLE ACTIONS
 *
 * ~tap_handle:
 *   1. output = {};          — kill tap reader
 *   2. fuse(w_copy_, r_copy_) — redirect w,r back together, killing A and B
 ******************************************************************************)

(* Kill tap output reader.
 *
 * Code: csp.h:632 — output = {};
 *
 * TLA:TapLifecycle.HandleKillOutput *)
HandleKillOutput ==
    /\ pc_handle = "alive"
    /\ tap_dead' = TRUE
    /\ pc_handle' = "fuse"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, fwd_tw, fused, pc_fwd>>

(* Fuse w_copy_ and r_copy_ back together.
 * This redirects the slots that B's writer and A's reader use,
 * killing those endpoints on the interposed channels.
 *
 * Code: csp.h:633 — fuse(w_copy_, r_copy_);
 *
 * TLA:TapLifecycle.HandleFuse *)
HandleFuse ==
    /\ pc_handle = "fuse"
    /\ b_dead' = TRUE      \* B's writer endpoint killed by slot redirect
    /\ a_dead' = TRUE      \* A's reader endpoint killed by slot redirect
    /\ fused' = TRUE
    /\ pc_handle' = "done"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, tap_dead,
                   fwd_tw, pc_fwd>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ ProducerSend
    \/ ForwarderReadDeath
    \/ ForwarderTap
    \/ ForwarderSkipTap
    \/ ForwarderWrite
    \/ ForwarderWriteFail
    \/ HandleKillOutput
    \/ HandleFuse

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ produced \in 0..MaxVals
    /\ consumed \in 0..MaxVals
    /\ tapped \in 0..MaxVals
    /\ fwd_buf \in 0..1
    /\ b_dead \in BOOLEAN
    /\ a_dead \in BOOLEAN
    /\ tap_dead \in BOOLEAN
    /\ fwd_tw \in BOOLEAN
    /\ fused \in BOOLEAN
    /\ pc_fwd \in {"read_wait", "tap_try", "write_wait", "exited"}
    /\ pc_handle \in {"alive", "fuse", "done"}

(* At most one value lost during fuse-back.  When the forwarder exits,
 * it may hold at most one value that was read from B but not yet
 * written to A.  This is the at-most-one-lost guarantee. *)
AtMostOneLost ==
    pc_fwd = "exited" => (produced - consumed <= 1)

(* Fuse kills channels: when handle destruction completes, the
 * interposed channels B and A must have dead endpoints.  This
 * ensures the forwarder can detect death and exit cleanly.
 *
 * In the correct design, w_copy_ and r_copy_ live in the handle,
 * so fuse() can kill them.  In the bug variant (copies in the
 * forwarder), fuse can't reach the copies and channels stay alive. *)
FuseKillsChannels ==
    pc_handle = "done" => (b_dead /\ a_dead)

====
