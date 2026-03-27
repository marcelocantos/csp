---- MODULE BlockingPoolLifecycle ----
(*******************************************************************************
 * Models the lazy-init double-check + shutdown coordination protocol from
 * csp/src/blocking_pool.cc.
 *
 * Two concerns are interleaved:
 *
 *   1. DOUBLE-CHECK INIT (ensure_started):
 *      Callers that want to submit work first call ensure_started, which uses
 *      a double-check pattern to avoid re-initialising the pool:
 *        - Fast path: check running (acquire). If true, done.
 *        - Slow path: acquire start_mu, re-check running (relaxed), then init.
 *      The re-check under start_mu is the critical safety step. Without it,
 *      two callers that both see running=false on the fast path would both
 *      proceed to init, growing threads_ twice (double-init).
 *
 *   2. SHUTDOWN COORDINATION:
 *      shutdown() uses the same start_mu double-check pattern (in reverse):
 *        - Fast path: check running (acquire). If false, done.
 *        - Slow path: acquire start_mu, re-check, then set stopping=true,
 *          notify workers, join threads, clear threads_, set running=false.
 *      Workers loop on: cv.wait(lk, []{!queue.empty || stopping}). They
 *      consume work items or exit when stopping=true and queue is empty.
 *
 * Participants:
 *   Caller1, Caller2  — two threads calling submit (→ ensure_started)
 *   Worker            — one pool thread running worker()
 *   Shutdown          — the shutdown() call
 *
 * Code references (src/blocking_pool.cc):
 *   EnsureCheck1       — line 14: running_.load(acquire) fast path
 *   EnsureAcquireMu    — line 16: lock_guard<mutex> lk(start_mu_)
 *   EnsureCheck2       — line 17: running_.load(relaxed) re-check
 *   EnsureInit         — lines 19-26: stopping=false, spawn threads
 *   EnsureSetRunning   — line 28: running_.store(true, release)
 *   SubmitLock         — line 52: lock_guard<mutex> lk(mu_)
 *   SubmitEnqueue      — line 53: queue_.push_back(...)
 *   SubmitNotify       — line 55: cv_.notify_one()
 *   WorkerWait         — line 63: cv_.wait(lk, predicate)
 *   WorkerTake         — lines 68-69: take item from queue
 *   WorkerRun          — lines 71-72: execute work + schedule
 *   ShutdownCheck1     — line 32: running_.load(acquire) fast path
 *   ShutdownAcquireMu  — line 34: lock_guard<mutex> lk(start_mu_)
 *   ShutdownCheck2     — line 35: running_.load(relaxed) re-check
 *   ShutdownLockQ      — line 38: lock_guard<mutex> lk(mu_)
 *   ShutdownSetStop    — line 39: stopping_.store(true, release)
 *   ShutdownUnlockQ    — line 40: } (end of lock_guard scope)
 *   ShutdownNotify     — line 41: cv_.notify_all()
 *   ShutdownJoin       — line 43: for (auto& t : threads_) t.join()
 *   ShutdownClear      — lines 44-45: threads_.clear(); queue_.clear()
 *   ShutdownClearRun   — line 46: running_.store(false, release)
 *
 * CV semantics:
 *   cv_.wait(lk, pred) expands to: while(!pred()) { cv_.wait(lk); }
 *   - pred() is evaluated with mu held
 *   - cv_.wait(lk) ATOMICALLY releases mu and blocks
 *   - On wake: reacquire mu, recheck pred
 *
 * Key invariants verified:
 *   NoDoubleInit    — threads_ is never initialised more than once per cycle
 *   NoSpuriousExit  — workers only exit when stopping=true
 *   WorkersSeeStop  — after shutdown completes, no worker is stuck blocked
 *                     without a pending wakeup
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Callers,    \* set of caller IDs, e.g. {c1, c2}
    MaxQueue    \* bound on queue depth (keeps state space finite)

VARIABLES
    running,        \* BOOLEAN: pool is live (atomic, acquire/release)
    stopping,       \* BOOLEAN: pool is draining (atomic)
    start_mu,       \* "none" | caller ID | "shutdown" — who holds start_mu
    mu,             \* "none" | caller ID | "worker" — who holds mu (queue mutex)
    queue,          \* Nat: number of items in queue (bounded)
    init_count,     \* Nat: how many times threads_ has been initialised this cycle
    worker_woken,   \* BOOLEAN: worker has a pending CV wake signal
    pc_caller,      \* [Callers -> pc]: per-caller program counter
    pc_worker,      \* worker thread program counter
    pc_shutdown     \* shutdown thread program counter

vars == <<running, stopping, start_mu, mu, queue, init_count,
          worker_woken, pc_caller, pc_worker, pc_shutdown>>

(*******************************************************************************
 * Caller PC states (submit path):
 *   "idle"           — not yet called submit
 *   "es_check1"      — about to do running fast-path check
 *   "es_acquire_mu"  — about to acquire start_mu
 *   "es_check2"      — inside start_mu, about to re-check running
 *   "es_init"        — performing init (stopping=false, spawn threads)
 *   "es_set_running" — about to store running=true
 *   "sub_lock"       — ensure_started done, about to lock mu
 *   "sub_enqueue"    — mu held, about to enqueue
 *   "sub_notify"     — item enqueued, about to notify_one
 *   "done"           — submit complete
 *
 * Worker PC states:
 *   "wait_pred"  — evaluating cv.wait predicate (mu held)
 *   "will_wait"  — pred was false, about to atomically release + block
 *   "blocked"    — sleeping in cv.wait
 *   "take"       — woken, pred true: taking item from queue
 *   "run"        — executing work item (mu released)
 *   "exited"     — worker loop exited (stopping=true, queue empty)
 *
 * Shutdown PC states:
 *   "idle"        — not started
 *   "sd_check1"   — about to do running fast-path check
 *   "sd_acquire"  — about to acquire start_mu
 *   "sd_check2"   — inside start_mu, about to re-check running
 *   "sd_lock_q"   — about to lock mu for stopping=true
 *   "sd_set_stop" — mu held, about to set stopping=true
 *   "sd_unlock_q" — about to release mu
 *   "sd_notify"   — about to notify_all
 *   "sd_join"     — about to join workers
 *   "sd_clear"    — joining done, about to clear threads_/queue_
 *   "sd_clr_run"  — about to store running=false
 *   "done"        — shutdown complete
 ******************************************************************************)

(*******************************************************************************
 * Initial state
 ******************************************************************************)
Init ==
    /\ running = FALSE
    /\ stopping = FALSE
    /\ start_mu = "none"
    /\ mu = "none"
    /\ queue = 0
    /\ init_count = 0
    /\ worker_woken = FALSE
    /\ pc_caller = [c \in Callers |-> "idle"]
    /\ pc_worker = "wait_pred"   \* Worker starts but will block immediately
    /\ pc_shutdown = "idle"

(*******************************************************************************
 * CALLER / ENSURE_STARTED ACTIONS
 ******************************************************************************)

(* Caller begins a submit call by entering ensure_started.
 *
 * Code: BlockingPool::submit -> ensure_started()  [line 50]
 *
 * TLA:BlockingPoolLifecycle.CallerBegin *)
CallerBegin(c) ==
    /\ pc_caller[c] = "idle"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_check1"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Fast-path check: if running, skip to submit.
 *
 * Code: if (running_.load(acquire)) return;  [line 14]
 *
 * TLA:BlockingPoolLifecycle.CallerEsCheck1 *)
CallerEsCheck1(c) ==
    /\ pc_caller[c] = "es_check1"
    /\ IF running
       THEN pc_caller' = [pc_caller EXCEPT ![c] = "sub_lock"]
       ELSE pc_caller' = [pc_caller EXCEPT ![c] = "es_acquire_mu"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Caller acquires start_mu for double-check.
 *
 * Code: lock_guard<mutex> lk(start_mu_);  [line 16]
 *
 * TLA:BlockingPoolLifecycle.CallerEsAcquireMu *)
CallerEsAcquireMu(c) ==
    /\ pc_caller[c] = "es_acquire_mu"
    /\ start_mu = "none"
    /\ start_mu' = c
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_check2"]
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Re-check running under start_mu. If true, someone else already inited.
 *
 * Code: if (running_.load(relaxed)) return;  [line 17]
 *
 * TLA:BlockingPoolLifecycle.CallerEsCheck2 *)
CallerEsCheck2(c) ==
    /\ pc_caller[c] = "es_check2"
    /\ start_mu = c
    /\ IF running
       THEN \* Already initialised — release lock and go to submit.
            /\ start_mu' = "none"
            /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_lock"]
       ELSE \* Not yet initialised — proceed with init.
            /\ start_mu' = start_mu   \* keep lock held
            /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_init"]
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Perform init: set stopping=false, spawn threads.
 * We model threads_ growth as incrementing init_count.
 * Spawning new threads is modelled by resetting pc_worker to "wait_pred"
 * — the fresh worker thread begins at the top of its loop.
 *
 * Code: stopping_.store(false); threads_.reserve(n); for(...) emplace_back  [lines 19-26]
 *
 * TLA:BlockingPoolLifecycle.CallerEsInit *)
CallerEsInit(c) ==
    /\ pc_caller[c] = "es_init"
    /\ start_mu = c
    /\ stopping' = FALSE
    /\ init_count' = init_count + 1
    /\ pc_worker' = "wait_pred"     \* model thread spawn: reset worker loop
    /\ worker_woken' = FALSE        \* new thread has no pending wakeup
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_set_running"]
    /\ UNCHANGED <<running, start_mu, mu, queue, pc_shutdown>>

(* Store running=true with release, then release start_mu.
 *
 * Code: running_.store(true, release);  [line 28]
 *       } // lock_guard releases start_mu_
 *
 * TLA:BlockingPoolLifecycle.CallerEsSetRunning *)
CallerEsSetRunning(c) ==
    /\ pc_caller[c] = "es_set_running"
    /\ start_mu = c
    /\ running' = TRUE
    /\ start_mu' = "none"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_lock"]
    /\ UNCHANGED <<stopping, mu, queue, init_count, worker_woken, pc_worker, pc_shutdown>>

(* Caller locks mu to enqueue work.
 *
 * Code: lock_guard<mutex> lk(mu_);  [line 52]
 *
 * TLA:BlockingPoolLifecycle.CallerSubLock *)
CallerSubLock(c) ==
    /\ pc_caller[c] = "sub_lock"
    /\ mu = "none"
    /\ mu' = c
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_enqueue"]
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Caller enqueues work item (bounded by MaxQueue).
 *
 * Code: queue_.push_back({imp, std::move(fn)});  [line 53]
 *
 * TLA:BlockingPoolLifecycle.CallerSubEnqueue *)
CallerSubEnqueue(c) ==
    /\ pc_caller[c] = "sub_enqueue"
    /\ mu = c
    /\ queue < MaxQueue
    /\ queue' = queue + 1
    /\ mu' = "none"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_notify"]
    /\ UNCHANGED <<running, stopping, start_mu, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Caller calls notify_one. Wakes worker if it is blocked.
 *
 * Code: cv_.notify_one();  [line 55]
 *
 * TLA:BlockingPoolLifecycle.CallerSubNotify *)
CallerSubNotify(c) ==
    /\ pc_caller[c] = "sub_notify"
    /\ worker_woken' = IF pc_worker = "blocked" THEN TRUE ELSE worker_woken
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "done"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   pc_worker, pc_shutdown>>

(*******************************************************************************
 * WORKER ACTIONS
 *
 * Worker loop:
 *   wait_pred -> (pred true?)
 *     yes: take -> run -> wait_pred
 *     no:  will_wait -> blocked -> (woken) -> wait_pred
 *   exited when stopping=true and queue=0
 ******************************************************************************)

(* Worker evaluates the CV predicate with mu held.
 * Pred: !queue_.empty() || stopping_.load(acquire)
 *
 * Code: cv_.wait(lk, [this]{ return !queue_.empty() || stopping_...; })  [line 63]
 *
 * TLA:BlockingPoolLifecycle.WorkerEvalPred *)
WorkerEvalPred ==
    /\ pc_worker = "wait_pred"
    /\ mu = "none"
    /\ mu' = "worker"     \* acquire mu to evaluate pred
    /\ IF (queue > 0) \/ stopping
       THEN pc_worker' = "take"       \* pred true: proceed to take/exit
       ELSE pc_worker' = "will_wait"  \* pred false: go to sleep
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

(* Worker is about to call cv.wait(lk): atomically release mu + block.
 *
 * Code: (inside cv_.wait): while(!pred) { cv.wait(lk); }  [line 63]
 *
 * TLA:BlockingPoolLifecycle.WorkerEnterWait *)
WorkerEnterWait ==
    /\ pc_worker = "will_wait"
    /\ mu = "worker"
    /\ mu' = "none"         \* atomically release mu + block
    /\ worker_woken' = FALSE
    /\ pc_worker' = "blocked"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   pc_caller, pc_shutdown>>

(* Worker is woken from cv.wait: reacquire mu, loop back to pred eval.
 *
 * Code: (cv.wait returns): reacquire lk, recheck pred  [line 63]
 *
 * TLA:BlockingPoolLifecycle.WorkerWoken *)
WorkerWoken ==
    /\ pc_worker = "blocked"
    /\ worker_woken
    /\ mu = "none"
    /\ mu' = "worker"
    /\ worker_woken' = FALSE
    /\ pc_worker' = "wait_pred"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   pc_caller, pc_shutdown>>

(* Worker takes an item from the queue (pred was true, queue non-empty).
 * If queue is empty and stopping=true, worker exits instead.
 *
 * Code: if (queue_.empty()) return;  // stopping  [line 67]
 *       w = std::move(queue_.back()); queue_.pop_back();  [lines 68-69]
 *
 * TLA:BlockingPoolLifecycle.WorkerTake *)
WorkerTake ==
    /\ pc_worker = "take"
    /\ mu = "worker"
    /\ IF queue = 0
       THEN \* queue empty: must be stopping
            /\ stopping = TRUE
            /\ mu' = "none"
            /\ pc_worker' = "exited"
            /\ UNCHANGED queue
       ELSE \* take one item
            /\ queue' = queue - 1
            /\ mu' = "none"
            /\ pc_worker' = "run"
    /\ UNCHANGED <<running, stopping, start_mu, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

(* Worker executes the work item and reschedules the imp.
 *
 * Code: w.fn(); w.imp->schedule();  [lines 71-72]
 *
 * TLA:BlockingPoolLifecycle.WorkerRun *)
WorkerRun ==
    /\ pc_worker = "run"
    /\ pc_worker' = "wait_pred"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

(*******************************************************************************
 * SHUTDOWN ACTIONS
 *
 * Shutdown mirrors ensure_started with a start_mu double-check, then:
 *   - locks mu, sets stopping=true, unlocks mu
 *   - notify_all workers
 *   - join workers (wait for exited)
 *   - clear threads_, queue_
 *   - store running=false
 ******************************************************************************)

(* Shutdown begins its fast-path running check.
 *
 * Code: if (!running_.load(acquire)) return;  [line 32]
 *
 * TLA:BlockingPoolLifecycle.ShutdownBegin *)
ShutdownBegin ==
    /\ pc_shutdown = "idle"
    /\ pc_shutdown' = "sd_check1"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Fast-path check.
 *
 * TLA:BlockingPoolLifecycle.ShutdownCheck1 *)
ShutdownCheck1 ==
    /\ pc_shutdown = "sd_check1"
    /\ IF ~running
       THEN pc_shutdown' = "done"      \* nothing to do
       ELSE pc_shutdown' = "sd_acquire"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown acquires start_mu.
 *
 * Code: lock_guard<mutex> lk(start_mu_);  [line 34]
 *
 * TLA:BlockingPoolLifecycle.ShutdownAcquireMu *)
ShutdownAcquireMu ==
    /\ pc_shutdown = "sd_acquire"
    /\ start_mu = "none"
    /\ start_mu' = "shutdown"
    /\ pc_shutdown' = "sd_check2"
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Re-check running under start_mu.
 *
 * Code: if (!running_.load(relaxed)) return;  [line 35]
 *
 * TLA:BlockingPoolLifecycle.ShutdownCheck2 *)
ShutdownCheck2 ==
    /\ pc_shutdown = "sd_check2"
    /\ start_mu = "shutdown"
    /\ IF ~running
       THEN \* Already shut down — release lock.
            /\ start_mu' = "none"
            /\ pc_shutdown' = "done"
       ELSE
            /\ start_mu' = start_mu    \* keep lock held
            /\ pc_shutdown' = "sd_lock_q"
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown locks mu to set stopping=true.
 *
 * Code: lock_guard<mutex> lk(mu_);  [line 38]
 *
 * TLA:BlockingPoolLifecycle.ShutdownLockQ *)
ShutdownLockQ ==
    /\ pc_shutdown = "sd_lock_q"
    /\ start_mu = "shutdown"
    /\ mu = "none"
    /\ mu' = "shutdown"
    /\ pc_shutdown' = "sd_set_stop"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown sets stopping=true with mu held.
 *
 * Code: stopping_.store(true, release);  [line 39]
 *
 * TLA:BlockingPoolLifecycle.ShutdownSetStop *)
ShutdownSetStop ==
    /\ pc_shutdown = "sd_set_stop"
    /\ mu = "shutdown"
    /\ stopping' = TRUE
    /\ pc_shutdown' = "sd_unlock_q"
    /\ UNCHANGED <<running, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown releases mu.
 *
 * Code: }  // end lock_guard scope  [line 40]
 *
 * TLA:BlockingPoolLifecycle.ShutdownUnlockQ *)
ShutdownUnlockQ ==
    /\ pc_shutdown = "sd_unlock_q"
    /\ mu = "shutdown"
    /\ mu' = "none"
    /\ pc_shutdown' = "sd_notify"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown calls notify_all. Only workers in "blocked" state are woken.
 * Workers in "will_wait" are still holding mu, so they cannot be in
 * "blocked" — they finish entering wait after mu is released.
 * The mu acquire/release above ensures: when we get here, any worker
 * that saw the old pred=false has either already blocked or will
 * recheck the pred (which is now true) before sleeping.
 *
 * Code: cv_.notify_all();  [line 41]
 *
 * TLA:BlockingPoolLifecycle.ShutdownNotify *)
ShutdownNotify ==
    /\ pc_shutdown = "sd_notify"
    /\ worker_woken' = IF pc_worker = "blocked" THEN TRUE ELSE worker_woken
    /\ pc_shutdown' = "sd_join"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   pc_caller, pc_worker>>

(* Shutdown waits for all worker threads to finish (join).
 * The worker must have exited for join to complete.
 *
 * Code: for (auto& t : threads_) t.join();  [line 43]
 *
 * TLA:BlockingPoolLifecycle.ShutdownJoin *)
ShutdownJoin ==
    /\ pc_shutdown = "sd_join"
    /\ pc_worker = "exited"
    /\ pc_shutdown' = "sd_clear"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

(* Shutdown clears threads_ and queue_ (model: reset init_count, drain queue).
 *
 * Code: threads_.clear(); queue_.clear();  [lines 44-45]
 *
 * TLA:BlockingPoolLifecycle.ShutdownClear *)
ShutdownClear ==
    /\ pc_shutdown = "sd_clear"
    /\ init_count' = 0
    /\ queue' = 0
    /\ pc_shutdown' = "sd_clr_run"
    /\ UNCHANGED <<running, stopping, start_mu, mu, worker_woken,
                   pc_caller, pc_worker>>

(* Shutdown stores running=false, releases start_mu.
 *
 * Code: running_.store(false, release);  [line 46]
 *       }  // lock_guard releases start_mu_
 *
 * TLA:BlockingPoolLifecycle.ShutdownClearRun *)
ShutdownClearRun ==
    /\ pc_shutdown = "sd_clr_run"
    /\ start_mu = "shutdown"
    /\ running' = FALSE
    /\ start_mu' = "none"
    /\ pc_shutdown' = "done"
    /\ UNCHANGED <<stopping, mu, queue, init_count, worker_woken,
                   pc_caller, pc_worker>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E c \in Callers :
        \/ CallerBegin(c)
        \/ CallerEsCheck1(c)
        \/ CallerEsAcquireMu(c)
        \/ CallerEsCheck2(c)
        \/ CallerEsInit(c)
        \/ CallerEsSetRunning(c)
        \/ CallerSubLock(c)
        \/ CallerSubEnqueue(c)
        \/ CallerSubNotify(c)
    \/ WorkerEvalPred
    \/ WorkerEnterWait
    \/ WorkerWoken
    \/ WorkerTake
    \/ WorkerRun
    \/ ShutdownBegin
    \/ ShutdownCheck1
    \/ ShutdownAcquireMu
    \/ ShutdownCheck2
    \/ ShutdownLockQ
    \/ ShutdownSetStop
    \/ ShutdownUnlockQ
    \/ ShutdownNotify
    \/ ShutdownJoin
    \/ ShutdownClear
    \/ ShutdownClearRun

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

CallerPCs == {"idle", "es_check1", "es_acquire_mu", "es_check2",
              "es_init", "es_set_running",
              "sub_lock", "sub_enqueue", "sub_notify", "done"}

WorkerPCs == {"wait_pred", "will_wait", "blocked", "take", "run", "exited"}

ShutdownPCs == {"idle", "sd_check1", "sd_acquire", "sd_check2",
                "sd_lock_q", "sd_set_stop", "sd_unlock_q", "sd_notify",
                "sd_join", "sd_clear", "sd_clr_run", "done"}

(* Type invariant. *)
TypeOK ==
    /\ running \in BOOLEAN
    /\ stopping \in BOOLEAN
    /\ start_mu \in ({"none", "shutdown"} \cup Callers)
    /\ mu \in ({"none", "worker", "shutdown"} \cup Callers)
    /\ queue \in 0..MaxQueue
    /\ init_count \in Nat
    /\ worker_woken \in BOOLEAN
    /\ pc_caller \in [Callers -> CallerPCs]
    /\ pc_worker \in WorkerPCs
    /\ pc_shutdown \in ShutdownPCs

(* Safety: threads_ is initialised at most once per running=true cycle.
 * No two callers can both perform es_init in the same init cycle, because
 * the second to acquire start_mu will see running=true on re-check. *)
NoDoubleInit ==
    init_count <= 1

(* Safety: workers only exit when stopping=true. *)
NoSpuriousExit ==
    pc_worker = "exited" => stopping

(* Safety: after shutdown completes notify, any blocked worker has been woken.
 * The mu acquire/release before notify_all ensures the worker has fully
 * entered cv.wait before the notification is sent. *)
WorkersSeeStop ==
    pc_shutdown = "sd_join" => (pc_worker = "blocked" => worker_woken)

(* Safety: running is only true when init_count = 1, UNLESS shutdown is in
 * the transient window between ShutdownClear (init_count=0) and
 * ShutdownClearRun (running=false). Outside that window the pool is either
 * fully initialised (running=true, init_count=1) or fully down
 * (running=false, init_count=0). *)
RunningConsistent ==
    (running /\ pc_shutdown \notin {"sd_clr_run"}) => init_count = 1

====
