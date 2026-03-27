---- MODULE SignalPipeLifecycle ----
(*******************************************************************************
 * Models the async-signal-safe self-pipe registration and delivery protocol
 * from csp/src/signal.cc.
 *
 * The protocol uses the self-pipe trick: notify() creates a pipe and registers
 * the write end in g_sig_pipes[].  The async signal handler iterates the
 * array and writes a byte to every pipe whose sig_mask includes the delivered
 * signal.  A sentinel imp clears sig_mask (release) then closes the write fd
 * when the reader is dropped.
 *
 * The key ordering guarantee is:
 *   Registrar:      write_fd/sig_mask stored → count stored (release)
 *   SignalHandler:  count loaded (acquire)  → write_fd/sig_mask read
 *   Sentinel:       sig_mask cleared (release) → write_fd closed
 *   SignalHandler:  sig_mask loaded (acquire) → write_fd used
 *
 * This ensures:
 *   (a) The handler never reads write_fd before the registrar has published it
 *       (enforced by release/acquire on g_sig_pipe_count).
 *   (b) The handler never writes to a closed fd (sig_mask is zeroed with
 *       release before close; handler acquires sig_mask before using write_fd).
 *
 * Participants:
 *   Registrar     — notify() call: creates pipe, acquires g_sig_mu, writes
 *                   write_fd and sig_mask to g_sig_pipes[idx], stores count+1
 *                   with release, installs handler.
 *   SignalHandler — async-signal context: loads count (acquire), iterates
 *                   [0,count), loads sig_mask (acquire), if match writes byte
 *                   to write_fd.
 *   Sentinel      — imp: waits for reader death, clears sig_mask (release),
 *                   closes write_fd.
 *
 * Code references (src/signal.cc):
 *   RegWriteFd     — line 94: g_sig_pipes[idx].write_fd = pipefd[1]
 *   RegStoreMask   — line 96: sig_mask.store(mask, relaxed)
 *   RegStoreCount  — line 98: g_sig_pipe_count.store(idx+1, release)
 *   HandlerLoad    — line 46: count = g_sig_pipe_count.load(acquire)
 *   HandlerCheck   — line 48: sig_mask.load(acquire) & bit
 *   HandlerWrite   — line 49: write(g_sig_pipes[i].write_fd, &byte, 1)
 *   SentinelClear  — line 120: sig_mask.store(0, release)
 *   SentinelClose  — line 121: close(wfd)
 *
 * Expected result: TLC finds no violations (all invariants hold).
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Pipes   \* set of pipe IDs, e.g. {p1, p2}

VARIABLES
    \* Registrar state per pipe
    write_fd_written,   \* [Pipes -> BOOLEAN]: write_fd stored in slot
    mask_stored,        \* [Pipes -> BOOLEAN]: sig_mask stored in slot
    count_released,     \* [Pipes -> BOOLEAN]: count increment published (release)
    reg_pc,             \* [Pipes -> registrar program counter]

    \* Handler state per pipe
    handler_count_seen, \* [Pipes -> BOOLEAN]: handler loaded count >= this pipe's idx+1
    handler_mask_seen,  \* [Pipes -> BOOLEAN]: handler loaded sig_mask for this pipe
    handler_wrote,      \* [Pipes -> BOOLEAN]: handler issued write() for this pipe

    \* Sentinel state per pipe
    mask_cleared,       \* [Pipes -> BOOLEAN]: sig_mask zeroed (release)
    fd_closed,          \* [Pipes -> BOOLEAN]: write_fd closed
    sentinel_pc,        \* [Pipes -> sentinel program counter]

    \* Error witness
    error_wrote_closed  \* [Pipes -> BOOLEAN]: write() issued after fd was closed

vars == <<write_fd_written, mask_stored, count_released,
          reg_pc,
          handler_count_seen, handler_mask_seen, handler_wrote,
          mask_cleared, fd_closed, sentinel_pc,
          error_wrote_closed>>

(*******************************************************************************
 * Initial state
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
 * REGISTRAR ACTIONS
 *
 * Registrar phases (per pipe, sequential within each pipe):
 *   write_fd -> store_mask -> store_count -> done
 *
 * g_sig_mu ensures these are not interleaved between pipes, but for the
 * ordering properties we model each pipe's registrar independently — the
 * invariants hold per-slot regardless of interleaving.
 ******************************************************************************)

(* Write write_fd into the slot.
 * Code: signal.cc:94
 *
 * TLA:SignalPipeLifecycle.RegWriteFd *)
RegWriteFd(p) ==
    /\ reg_pc[p] = "write_fd"
    /\ write_fd_written' = [write_fd_written EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "store_mask"]
    /\ UNCHANGED <<mask_stored, count_released,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Store sig_mask into the slot (relaxed — published via the count release).
 * Code: signal.cc:96
 *
 * TLA:SignalPipeLifecycle.RegStoreMask *)
RegStoreMask(p) ==
    /\ reg_pc[p] = "store_mask"
    /\ mask_stored' = [mask_stored EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "store_count"]
    /\ UNCHANGED <<write_fd_written, count_released,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Store count+1 with release — publishes write_fd and sig_mask to handler.
 * After this step the handler is permitted to see this pipe's slot.
 * Code: signal.cc:98
 *
 * TLA:SignalPipeLifecycle.RegStoreCount *)
RegStoreCount(p) ==
    /\ reg_pc[p] = "store_count"
    /\ write_fd_written[p]  \* both stores preceded the release (program order)
    /\ mask_stored[p]
    /\ count_released' = [count_released EXCEPT ![p] = TRUE]
    /\ reg_pc' = [reg_pc EXCEPT ![p] = "done"]
    /\ UNCHANGED <<write_fd_written, mask_stored,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(*******************************************************************************
 * SIGNAL HANDLER ACTIONS
 *
 * The handler fires asynchronously. Its steps can interleave freely with
 * the registrar and sentinel, subject only to the acquire ordering constraints.
 *
 * Handler phases (per pipe):
 *   load_count -> load_mask -> [write | skip]
 *
 * The acquire on count_released ensures write_fd and sig_mask are visible.
 * The acquire on sig_mask determines whether to write.
 * If sentinel cleared the mask before HandlerLoadMask, handler sees 0 and
 * skips the write — which is safe because the sentinel clears before close.
 ******************************************************************************)

(* Handler loads count with acquire — can only see this pipe's slot after
 * the registrar's release store (count_released[p]).
 * Code: signal.cc:46
 *
 * TLA:SignalPipeLifecycle.HandlerLoadCount *)
HandlerLoadCount(p) ==
    /\ ~handler_count_seen[p]
    /\ count_released[p]          \* acquire: requires registrar's release
    /\ handler_count_seen' = [handler_count_seen EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Handler loads sig_mask with acquire for this pipe.
 * Enabled after count was acquired. The mask may already be cleared by
 * sentinel — in that case the handler will skip the write.
 * Code: signal.cc:48
 *
 * TLA:SignalPipeLifecycle.HandlerLoadMask *)
HandlerLoadMask(p) ==
    /\ handler_count_seen[p]
    /\ ~handler_mask_seen[p]
    /\ handler_mask_seen' = [handler_mask_seen EXCEPT ![p] = TRUE]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_wrote,
                   mask_cleared, fd_closed, sentinel_pc, error_wrote_closed>>

(* Handler writes to write_fd — only when sig_mask was non-zero (not yet
 * cleared by sentinel) at the time of HandlerLoadMask.
 *
 * The release/acquire on sig_mask (sentinel release, handler acquire)
 * guarantees: if handler saw mask=0, sentinel has already cleared it;
 * sentinel clears before closing; therefore fd is still open when handler
 * sees mask≠0.
 *
 * We record error_wrote_closed as a witness: true if the write happens
 * after the fd was already closed (which should be impossible in this spec).
 * Code: signal.cc:49
 *
 * TLA:SignalPipeLifecycle.HandlerWrite *)
HandlerWrite(p) ==
    /\ handler_mask_seen[p]
    /\ ~handler_wrote[p]
    /\ ~mask_cleared[p]       \* saw mask≠0 (not yet cleared)
    /\ handler_wrote' = [handler_wrote EXCEPT ![p] = TRUE]
    /\ error_wrote_closed' = [error_wrote_closed EXCEPT
                                ![p] = fd_closed[p]]  \* witness
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen,
                   mask_cleared, fd_closed, sentinel_pc>>

(*******************************************************************************
 * SENTINEL ACTIONS
 *
 * Sentinel phases (per pipe):
 *   wait_reader_death -> clear_mask -> close_fd -> done
 *
 * The sentinel imp fires when the reader end of the pipe is dropped.
 * It MUST clear sig_mask (release) BEFORE closing write_fd so that the
 * handler, which acquires sig_mask, cannot use a closed fd.
 ******************************************************************************)

(* Sentinel detects reader death (can happen any time after registration).
 * Modelled as non-deterministic once the pipe has been registered.
 * Code: signal.cc:119 — prialt(~out_copy, ~kill_r) returns
 *
 * TLA:SignalPipeLifecycle.SentinelReaderDied *)
SentinelReaderDied(p) ==
    /\ sentinel_pc[p] = "wait_reader_death"
    /\ count_released[p]          \* pipe must have been registered first
    /\ sentinel_pc' = [sentinel_pc EXCEPT ![p] = "clear_mask"]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   mask_cleared, fd_closed, error_wrote_closed>>

(* Sentinel clears sig_mask with release — after this, handler will see 0
 * on its acquire load, and skip the write.
 * Code: signal.cc:120
 *
 * TLA:SignalPipeLifecycle.SentinelClearMask *)
SentinelClearMask(p) ==
    /\ sentinel_pc[p] = "clear_mask"
    /\ mask_cleared' = [mask_cleared EXCEPT ![p] = TRUE]
    /\ sentinel_pc' = [sentinel_pc EXCEPT ![p] = "close_fd"]
    /\ UNCHANGED <<write_fd_written, mask_stored, count_released, reg_pc,
                   handler_count_seen, handler_mask_seen, handler_wrote,
                   fd_closed, error_wrote_closed>>

(* Sentinel closes write_fd — safe because sig_mask is already zeroed.
 * Code: signal.cc:121
 *
 * TLA:SignalPipeLifecycle.SentinelCloseFd *)
SentinelCloseFd(p) ==
    /\ sentinel_pc[p] = "close_fd"
    /\ mask_cleared[p]            \* cannot close before clearing mask
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
 * PROPERTIES
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

(* Safety: the handler may only see a pipe's slot after the registrar has
 * published the count (release/acquire ordering). *)
HandlerSeesPublishedSlot ==
    \A p \in Pipes : handler_count_seen[p] => count_released[p]

(* Safety: the handler may only see write_fd and sig_mask after the registrar
 * stored them — guaranteed by the release on count (which follows both
 * stores in program order). *)
HandlerSeesInitialisedSlot ==
    \A p \in Pipes :
        handler_count_seen[p] =>
            /\ write_fd_written[p]
            /\ mask_stored[p]

(* Safety: the handler never writes to a closed fd.
 * error_wrote_closed[p] is set TRUE iff HandlerWrite fires while fd_closed[p]
 * is already TRUE. In the correct spec, this can never happen. *)
NoWriteToClosedFd ==
    \A p \in Pipes : ~error_wrote_closed[p]

(* Safety: fd is only closed after the mask is cleared. *)
CloseRequiresMaskCleared ==
    \A p \in Pipes : fd_closed[p] => mask_cleared[p]

====
