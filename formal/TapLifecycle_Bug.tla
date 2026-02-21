---- MODULE TapLifecycle_Bug ----
(*******************************************************************************
 * Bug variant of TapLifecycle: forwarder holds w_copy/r_copy instead
 * of handle, creating a reference cycle.
 *
 * When the forwarder imp holds copies of w and r (w_copy_, r_copy_),
 * these copies keep the intermediate channels B and A alive via
 * their slot refcounts.  When the handle tries to fuse, the slot
 * redirects succeed but the forwarder's copies maintain endpoint
 * liveness on B and A.  The forwarder never sees channel death,
 * blocking forever — a reference-cycle deadlock.
 *
 * Expected TLC result: FuseKillsChannels violation.
 *
 * Concrete trace:
 *   1. HandleKillOutput: tap reader dies
 *   2. HandleFuse (BUG): fused=TRUE but b_dead=FALSE, a_dead=FALSE
 *      (forwarder's copies keep channels alive)
 *   3. pc_handle="done": FuseKillsChannels violated
 ******************************************************************************)

EXTENDS Integers

CONSTANTS MaxVals

VARIABLES
    produced, consumed, tapped, fwd_buf, b_dead, a_dead,
    tap_dead, fwd_tw, fused, pc_fwd, pc_handle

vars == <<produced, consumed, tapped, fwd_buf, b_dead, a_dead,
          tap_dead, fwd_tw, fused, pc_fwd, pc_handle>>

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

\* ---- FORWARDER ----

ProducerSend ==
    /\ pc_fwd = "read_wait"
    /\ produced < MaxVals
    /\ ~b_dead
    /\ ~a_dead
    /\ produced' = produced + 1
    /\ fwd_buf' = 1
    /\ pc_fwd' = "tap_try"
    /\ UNCHANGED <<consumed, tapped, b_dead, a_dead, tap_dead,
                   fwd_tw, fused, pc_handle>>

ForwarderReadDeath ==
    /\ pc_fwd = "read_wait"
    /\ (b_dead \/ a_dead)
    /\ pc_fwd' = "exited"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fwd_tw, fused, pc_handle>>

ForwarderTap ==
    /\ pc_fwd = "tap_try"
    /\ fwd_tw
    /\ ~tap_dead
    /\ tapped' = tapped + 1
    /\ pc_fwd' = "write_wait"
    /\ UNCHANGED <<produced, consumed, fwd_buf, b_dead, a_dead,
                   tap_dead, fwd_tw, fused, pc_handle>>

ForwarderSkipTap ==
    /\ pc_fwd = "tap_try"
    /\ (tap_dead \/ ~fwd_tw)
    /\ fwd_tw' = FALSE
    /\ pc_fwd' = "write_wait"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fused, pc_handle>>

ForwarderWrite ==
    /\ pc_fwd = "write_wait"
    /\ ~a_dead
    /\ consumed' = consumed + 1
    /\ fwd_buf' = 0
    /\ pc_fwd' = "read_wait"
    /\ UNCHANGED <<produced, tapped, b_dead, a_dead, tap_dead,
                   fwd_tw, fused, pc_handle>>

ForwarderWriteFail ==
    /\ pc_fwd = "write_wait"
    /\ a_dead
    /\ pc_fwd' = "exited"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fwd_tw, fused, pc_handle>>

\* ---- HANDLE ----

HandleKillOutput ==
    /\ pc_handle = "alive"
    /\ tap_dead' = TRUE
    /\ pc_handle' = "fuse"
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, fwd_tw, fused, pc_fwd>>

(* BUG: fuse completes but b_dead and a_dead stay FALSE.
 * The forwarder's copies of w and r keep the intermediate
 * channels' endpoint groups alive, creating a reference cycle. *)
HandleFuse ==
    /\ pc_handle = "fuse"
    /\ fused' = TRUE
    /\ pc_handle' = "done"
    \* BUG: b_dead and a_dead NOT set to TRUE
    /\ UNCHANGED <<produced, consumed, tapped, fwd_buf, b_dead,
                   a_dead, tap_dead, fwd_tw, pc_fwd>>

\* ---- SPEC ----

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

\* ---- PROPERTIES ----

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

FuseKillsChannels ==
    pc_handle = "done" => (b_dead /\ a_dead)

====
