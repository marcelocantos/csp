---- MODULE CancelReasonPublication_Bug ----
(* The companion configuration selects the old exchange(true)-before-write
   ordering. TLC must fail SafeRead: a poller copies an unwritten reason. *)
EXTENDS CancelReasonPublication
====
