---- MODULE BlockingPoolLifecycle_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the missing re-check race in ensure_started.
 *
 * In this version, ensure_started does NOT re-check running inside start_mu.
 * After acquiring start_mu, it proceeds directly to init:
 *
 *   void ensure_started() {                    // BUGGY
 *       if (running_.load(acquire)) return;    // fast path — OK
 *       lock_guard lk(start_mu_);
 *       // BUG: no re-check here
 *       stopping_ = false;
 *       // ... spawn threads ...
 *       running_.store(true, release);
 *   }
 *
 * This allows the following dangerous interleaving with two callers:
 *
 *   Caller1: fast-path check → running=false (decides to init)
 *   Caller2: fast-path check → running=false (decides to init)
 *   Caller1: acquires start_mu
 *   Caller1: performs init (init_count becomes 1)
 *   Caller1: stores running=true, releases start_mu
 *   Caller2: acquires start_mu  ← correct code would re-check and return here
 *   Caller2: performs init again (init_count becomes 2) ← DOUBLE INIT!
 *   Caller2: stores running=true, releases start_mu
 *
 * With the fix (re-check under start_mu): Caller2 sees running=true after
 * acquiring start_mu and returns early, so init_count stays at 1.
 *
 * Expected result: TLC finds a NoDoubleInit violation (init_count = 2).
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Callers,
    MaxQueue

VARIABLES
    running,
    stopping,
    start_mu,
    mu,
    queue,
    init_count,
    worker_woken,
    pc_caller,
    pc_worker,
    pc_shutdown

vars == <<running, stopping, start_mu, mu, queue, init_count,
          worker_woken, pc_caller, pc_worker, pc_shutdown>>

Init ==
    /\ running = FALSE
    /\ stopping = FALSE
    /\ start_mu = "none"
    /\ mu = "none"
    /\ queue = 0
    /\ init_count = 0
    /\ worker_woken = FALSE
    /\ pc_caller = [c \in Callers |-> "idle"]
    /\ pc_worker = "wait_pred"
    /\ pc_shutdown = "idle"

(*******************************************************************************
 * CALLER ACTIONS — BUG: ensure_started has no re-check under start_mu.
 *
 * CallerEsCheck2 is REMOVED. After acquiring start_mu, callers proceed
 * directly to es_init, even if running is already true.
 ******************************************************************************)

CallerBegin(c) ==
    /\ pc_caller[c] = "idle"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_check1"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* Fast-path check. Still present — this is not the bug. *)
CallerEsCheck1(c) ==
    /\ pc_caller[c] = "es_check1"
    /\ IF running
       THEN pc_caller' = [pc_caller EXCEPT ![c] = "sub_lock"]
       ELSE pc_caller' = [pc_caller EXCEPT ![c] = "es_acquire_mu"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

CallerEsAcquireMu(c) ==
    /\ pc_caller[c] = "es_acquire_mu"
    /\ start_mu = "none"
    /\ start_mu' = c
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_init"]   \* BUG: skip to init
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

(* BUG: no CallerEsCheck2 — we go straight to init without re-checking running *)

CallerEsInit(c) ==
    /\ pc_caller[c] = "es_init"
    /\ start_mu = c
    /\ stopping' = FALSE
    /\ init_count' = init_count + 1    \* This can now reach 2
    /\ pc_worker' = "wait_pred"        \* model thread spawn: reset worker loop
    /\ worker_woken' = FALSE           \* new thread has no pending wakeup
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "es_set_running"]
    /\ UNCHANGED <<running, start_mu, mu, queue, pc_shutdown>>

CallerEsSetRunning(c) ==
    /\ pc_caller[c] = "es_set_running"
    /\ start_mu = c
    /\ running' = TRUE
    /\ start_mu' = "none"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_lock"]
    /\ UNCHANGED <<stopping, mu, queue, init_count, worker_woken, pc_worker, pc_shutdown>>

CallerSubLock(c) ==
    /\ pc_caller[c] = "sub_lock"
    /\ mu = "none"
    /\ mu' = c
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_enqueue"]
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

CallerSubEnqueue(c) ==
    /\ pc_caller[c] = "sub_enqueue"
    /\ mu = c
    /\ queue < MaxQueue
    /\ queue' = queue + 1
    /\ mu' = "none"
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "sub_notify"]
    /\ UNCHANGED <<running, stopping, start_mu, init_count,
                   worker_woken, pc_worker, pc_shutdown>>

CallerSubNotify(c) ==
    /\ pc_caller[c] = "sub_notify"
    /\ worker_woken' = IF pc_worker = "blocked" THEN TRUE ELSE worker_woken
    /\ pc_caller' = [pc_caller EXCEPT ![c] = "done"]
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   pc_worker, pc_shutdown>>

(*******************************************************************************
 * WORKER ACTIONS — identical to the correct version.
 ******************************************************************************)

WorkerEvalPred ==
    /\ pc_worker = "wait_pred"
    /\ mu = "none"
    /\ mu' = "worker"
    /\ IF (queue > 0) \/ stopping
       THEN pc_worker' = "take"
       ELSE pc_worker' = "will_wait"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

WorkerEnterWait ==
    /\ pc_worker = "will_wait"
    /\ mu = "worker"
    /\ mu' = "none"
    /\ worker_woken' = FALSE
    /\ pc_worker' = "blocked"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   pc_caller, pc_shutdown>>

WorkerWoken ==
    /\ pc_worker = "blocked"
    /\ worker_woken
    /\ mu = "none"
    /\ mu' = "worker"
    /\ worker_woken' = FALSE
    /\ pc_worker' = "wait_pred"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   pc_caller, pc_shutdown>>

WorkerTake ==
    /\ pc_worker = "take"
    /\ mu = "worker"
    /\ IF queue = 0
       THEN /\ stopping = TRUE
            /\ mu' = "none"
            /\ pc_worker' = "exited"
            /\ UNCHANGED queue
       ELSE /\ queue' = queue - 1
            /\ mu' = "none"
            /\ pc_worker' = "run"
    /\ UNCHANGED <<running, stopping, start_mu, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

WorkerRun ==
    /\ pc_worker = "run"
    /\ pc_worker' = "wait_pred"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_shutdown>>

(*******************************************************************************
 * SHUTDOWN ACTIONS — identical to the correct version.
 ******************************************************************************)

ShutdownBegin ==
    /\ pc_shutdown = "idle"
    /\ pc_shutdown' = "sd_check1"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownCheck1 ==
    /\ pc_shutdown = "sd_check1"
    /\ IF ~running
       THEN pc_shutdown' = "done"
       ELSE pc_shutdown' = "sd_acquire"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownAcquireMu ==
    /\ pc_shutdown = "sd_acquire"
    /\ start_mu = "none"
    /\ start_mu' = "shutdown"
    /\ pc_shutdown' = "sd_check2"
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownCheck2 ==
    /\ pc_shutdown = "sd_check2"
    /\ start_mu = "shutdown"
    /\ IF ~running
       THEN /\ start_mu' = "none"
            /\ pc_shutdown' = "done"
       ELSE /\ start_mu' = start_mu
            /\ pc_shutdown' = "sd_lock_q"
    /\ UNCHANGED <<running, stopping, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownLockQ ==
    /\ pc_shutdown = "sd_lock_q"
    /\ start_mu = "shutdown"
    /\ mu = "none"
    /\ mu' = "shutdown"
    /\ pc_shutdown' = "sd_set_stop"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownSetStop ==
    /\ pc_shutdown = "sd_set_stop"
    /\ mu = "shutdown"
    /\ stopping' = TRUE
    /\ pc_shutdown' = "sd_unlock_q"
    /\ UNCHANGED <<running, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownUnlockQ ==
    /\ pc_shutdown = "sd_unlock_q"
    /\ mu = "shutdown"
    /\ mu' = "none"
    /\ pc_shutdown' = "sd_notify"
    /\ UNCHANGED <<running, stopping, start_mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownNotify ==
    /\ pc_shutdown = "sd_notify"
    /\ worker_woken' = IF pc_worker = "blocked" THEN TRUE ELSE worker_woken
    /\ pc_shutdown' = "sd_join"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   pc_caller, pc_worker>>

ShutdownJoin ==
    /\ pc_shutdown = "sd_join"
    /\ pc_worker = "exited"
    /\ pc_shutdown' = "sd_clear"
    /\ UNCHANGED <<running, stopping, start_mu, mu, queue, init_count,
                   worker_woken, pc_caller, pc_worker>>

ShutdownClear ==
    /\ pc_shutdown = "sd_clear"
    /\ init_count' = 0
    /\ queue' = 0
    /\ pc_shutdown' = "sd_clr_run"
    /\ UNCHANGED <<running, stopping, start_mu, mu, worker_woken,
                   pc_caller, pc_worker>>

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

CallerPCs == {"idle", "es_check1", "es_acquire_mu",
              "es_init", "es_set_running",
              "sub_lock", "sub_enqueue", "sub_notify", "done"}

WorkerPCs == {"wait_pred", "will_wait", "blocked", "take", "run", "exited"}

ShutdownPCs == {"idle", "sd_check1", "sd_acquire", "sd_check2",
                "sd_lock_q", "sd_set_stop", "sd_unlock_q", "sd_notify",
                "sd_join", "sd_clear", "sd_clr_run", "done"}

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

(* This invariant is VIOLATED by the bug: two callers can both pass the
 * fast-path check and both acquire start_mu in turn, running init twice. *)
NoDoubleInit ==
    init_count <= 1

NoSpuriousExit ==
    pc_worker = "exited" => stopping

WorkersSeeStop ==
    pc_shutdown = "sd_join" => (pc_worker = "blocked" => worker_woken)

RunningConsistent ==
    (running /\ pc_shutdown \notin {"sd_clr_run"}) => init_count = 1

====
