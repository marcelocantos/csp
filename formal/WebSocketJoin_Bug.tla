---- MODULE WebSocketJoin_Bug ----
(* Waiting only for completion-channel death strands the cancellation
   exception's synchronous send; close never completes. *)
EXTENDS WebSocketClose
====
