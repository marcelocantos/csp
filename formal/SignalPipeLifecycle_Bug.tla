---- MODULE SignalPipeLifecycle_Bug ----
(*******************************************************************************
 * Bug variant of SignalPipeLifecycle: the count store is relaxed instead of
 * release.
 *
 * In the correct implementation (signal.cc:98), g_sig_pipe_count is stored
 * with memory_order_release. This pairs with the handler's
 * memory_order_acquire load (line 46) to establish a happens-before edge:
 * all stores by the registrar before the count increment (write_fd at line
 * 94, sig_mask at line 96) are guaranteed to be visible to the handler after
 * it acquires the count.
 *
 * Bug: if the count store uses memory_order_relaxed, the acquire load in the
 * handler provides no synchronisation with the registrar. The handler can
 * observe count = idx+1 (new slot visible) while write_fd and sig_mask are
 * still uninitialized — leading to a write() call on an uninitialized file
 * descriptor.
 *
 * This is modelled by removing the guard `count_released[p]` from
 * HandlerLoadCount: the handler may see the new count at any time after the
 * registrar begins writing the slot, rather than only after the release store
 * completes. Concretely: on a weakly ordered CPU, a relaxed store can become
 * globally visible out-of-order with respect to the preceding write_fd and
 * mask_stored stores, which may still be in the store buffer.
 *
 * The HandlerLoadMask action in the bug spec also no longer benefits from the
 * count acquire synchronisation. However, the sig_mask store itself (line 96)
 * is memory_order_relaxed in the implementation, so we additionally remove
 * the guarantee that mask_stored[p] is visible before HandlerLoadMask.
 *
 * In the absence of the release/acquire fence, the handler can proceed to
 * HandlerWrite while write_fd_written[p] is still FALSE — writing to an
 * uninitialised fd. Additionally, the handler can see a stale (non-zero)
 * sig_mask even after the sentinel has cleared it (if the sentinel's release
 * is not paired with an acquire — but in practice sig_mask itself has
 * release/acquire; the bug is only in the count). We model the primary hazard:
 * writing with an uninitialised fd.
 *
 * Violated invariants:
 *   HandlerSeesPublishedSlot  — handler sees count before registrar releases
 *   HandlerSeesInitialisedSlot — handler sees count before write_fd/mask stored
 *
 * Expected result: TLC finds a violation of HandlerSeesInitialisedSlot
 * (and HandlerSeesPublishedSlot).
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Pipes   \* set of pipe IDs, e.g. {p1, p2}

VARIABLES
    write_fd_written,
    mask_stored,
    count_released,
    reg_pc,
    handler_count_seen,
    handler_mask_seen,
    handler_wrote,
    mask_cleared,
    fd_closed,
    sentinel_pc,
    error_wrote_closed

vars == <<write_fd_written, mask_stored, count_released,
          reg_pc,
          handler_count_seen, handler_mask_seen, handler_wrote,
          mask_cleared, fd_closed, sentinel_pc,
          error_wrote_closed>>

(*******************************************************************************
 * Initial state — identical to the correct spec.
 ******************************************************************************)

Init ==
    /\ write_fd_written   = [p \in Pipes |-> FALSE]
    /\ mask_stored        = [p \in Pipes |-> FALSE]
    /\ count_released     = [p \in Pipes |-> FALSE]
    /\ reg_pc             = [p \in Pipes |-> "write_fd"]
    /\ handler_count_seen = [p \in Pipes |-> FALSE]
    /\ handler_mask_seen  = [p \in Pipes |-> FALSE]
    /\ handler_wrote      = [p \in Pipes |-> FALSE]
    /\ mask_cleared       = [p \in Pipes |-> FALSE]
    /\ fd_closed          = [p \in Pipes |-> FALSE]
    /\ sentinel_pc        = [p \in Pipes |-> "wait_reader_death"]
    /\ error_wrote_closed = [p \in Pipes |-> FALSE]

(*******************************************************************************
 * REGISTRAR ACTIONS — identical to the correct spec.
 ******************************************************************************)

RegWriteFd(p) ==
    /\ reg_pc[p] = "write_fd"
    /\ write_fd_written' = [write_fd_written EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "store_mask"]
    /\ UNCHANGED <<mask_stored, count_released,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

RegStoreMask(p) ==
    /\ reg_pc[p] = "store_mask"
    /\ mask_stored' = [mask_stored EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "store_count"]
    /\ UNCHANGED <<write_fd_written, count_released,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Registrar stores count with RELAXED ordering (the bug).
 * We still track count_released[p] = TRUE so SentinelReaderDied can gate
 * on registration being complete (the sentinel still can't fire before the
 * pipe is registered). But HandlerLoadCount below no longer requires it,
 * modelling the absence of the release/acquire fence. *)
RegStoreCount(p) ==
    /\ reg_pc[p] = "store_count"
    /\ count_released' = [count_released EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "done"]
    /\ UNCHANGED <<write_fd_written, mask_stored,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(*******************************************************************************
 * SIGNAL HANDLER ACTIONS — BUG VARIANT
 *
 * BUG: HandlerLoadCount no longer requires count_released[p].
 *
 * With a relaxed count store, the handler's acquire load on count provides
 * no synchronisation with the preceding write_fd and mask_stored stores.
 * We model this by allowing HandlerLoadCount to fire as soon as the
 * registrar has advanced past "write_fd" (the slot index is being written),
 * without waiting for the count release to complete.
 *
 * "reg_pc[p] # write_fd" means the registrar has started work on this slot
 * and the count value is being (or has been) written — the earliest point at
 * which a relaxed store could speculatively become visible.
 ******************************************************************************)

(* BUG: handler sees new count without requiring count_released[p].
 * Enabled as soon as the registrar has started the slot (reg_pc past write_fd).
 * Code: signal.cc:46 — but count stored relaxed, not release.
 *
 * TLA:SignalPipeLifecycle_Bug.HandlerLoadCount *)
HandlerLoadCount(p) ==
    /\ ~handler_count_seen[p]
    /\ reg_pc[p] # "write_fd"     \* registrar has started slot (no fence required)
    /\ handler_count_seen' = [handler_count_seen EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

HandlerLoadMask(p) ==
    /\ handler_count_seen[p]
    /\ ~handler_mask_seen[p]
    /\ handler_mask_seen' = [handler_mask_seen EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Handler writes — still only when mask seen non-zero and fd open.
 * The bug: write_fd may not be initialised yet (write_fd_written[p] = FALSE).
 * error_wrote_closed is FALSE here because fd can only be closed after
 * mask is cleared, which requires SentinelReaderDied which requires
 * count_released[p] — and the race is on uninitialized fd, not closed fd.
 * We track the write-to-uninit-fd hazard via HandlerSeesInitialisedSlot. *)
HandlerWrite(p) ==
    /\ handler_mask_seen[p]
    /\ ~handler_wrote[p]
    /\ ~mask_cleared[p]
    /\ handler_wrote' = [handler_wrote EXCEPT ![p] = TRUE]
    /\ error_wrote_closed' = [error_wrote_closed EXCEPT
                                ![p] = fd_closed[p]]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen,
                   mask_cleared, fd_closed, sentinel_pc>>

(*******************************************************************************
 * SENTINEL ACTIONS — identical to the correct spec.
 ******************************************************************************)

SentinelReaderDied(p) ==
    /\ sentinel_pc[p] = "wait_reader_death"
    /\ count_released[p]
    /\ sentinel_pc' = [sentinel_pc EXCEPT ![p] = "clear_mask"]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, error_wrote_closed>>

SentinelClearMask(p) ==
    /\ sentinel_pc[p] = "clear_mask"
    /\ mask_cleared' = [mask_cleared EXCEPT ![p] = TRUE]
    /\ sentinel_pc' = [sentinel_pc EXCEPT ![p] = "close_fd"]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   fd_closed, error_wrote_closed>>

SentinelCloseFd(p) ==
    /\ sentinel_pc[p] = "close_fd"
    /\ mask_cleared[p]
    /\ fd_closed' = [fd_closed EXCEPT ![p] = TRUE]
    /\ sentinel_pc' = [sentinel_pc EXCEPT ![p] = "done"]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, error_wrote_closed>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E p \in Pipes :
        \/ RegWriteFd(p)
        \/ RegStoreMask(p)
        \/ RegStoreCount(p)
        \/ HandlerLoadCount(p)
        \/ HandlerLoadMask(p)
        \/ HandlerWrite(p)
        \/ SentinelReaderDied(p)
        \/ SentinelClearMask(p)
        \/ SentinelCloseFd(p)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES — same invariants as the correct spec.
 ******************************************************************************)

TypeOK ==
    /\ write_fd_written   \in [Pipes -> BOOLEAN]
    /\ mask_stored        \in [Pipes -> BOOLEAN]
    /\ count_released     \in [Pipes -> BOOLEAN]
    /\ reg_pc             \in [Pipes -> {"write_fd", "store_mask", "store_count", "done"}]
    /\ handler_count_seen \in [Pipes -> BOOLEAN]
    /\ handler_mask_seen  \in [Pipes -> BOOLEAN]
    /\ handler_wrote      \in [Pipes -> BOOLEAN]
    /\ mask_cleared       \in [Pipes -> BOOLEAN]
    /\ fd_closed          \in [Pipes -> BOOLEAN]
    /\ sentinel_pc        \in [Pipes -> {"wait_reader_death", "clear_mask",
                                         "close_fd", "done"}]
    /\ error_wrote_closed \in [Pipes -> BOOLEAN]

(* VIOLATED by bug: handler sees count before registrar releases it. *)
HandlerSeesPublishedSlot ==
    \A p \in Pipes : handler_count_seen[p] => count_released[p]

(* VIOLATED by bug: handler can see count before write_fd and sig_mask
 * are stored — the primary hazard. *)
HandlerSeesInitialisedSlot ==
    \A p \in Pipes :
        handler_count_seen[p] =>
            /\ write_fd_written[p]
            /\ mask_stored[p]

NoWriteToClosedFd ==
    \A p \in Pipes : ~error_wrote_closed[p]

CloseRequiresMaskCleared ==
    \A p \in Pipes : fd_closed[p] => mask_cleared[p]

====
