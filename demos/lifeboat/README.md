# Lifeboat — Port Meridian

An interactive orbital dock driven by a live CSP application. Ships arrive,
two cranes compete for cargo, a warehouse feeds a fabricator, and a tram
carries supplies to the habitat. The browser observes the running C++ program.

## Run

From the repository root, with a C++20 compiler and Make installed:

```sh
make lifeboat
build/lifeboat/lifeboat
```

Open <http://127.0.0.1:8042>. No npm install, external assets, or submodule
initialization is needed: this consumer compiles the checked-in `dist/` core
and HTTP/1.1 files with the in-tree llhttp dependency.

Use `--port 8043` for another port, or `--assets PATH` when running outside
the repository root. `--help-agent` describes the HTTP controls. The server
binds to loopback by default. Stop it with Ctrl-C.

## Try this sequence

1. **Pause the fabricator** and **call a traffic surge**. Storage fills;
   cranes wait to hand over cargo; eventually arrival control waits too.
2. Enable **Channel view** to see the topology and actual stage populations.
   Select a structure to inspect its live status. Drag, scroll, or use the
   zoom controls to explore. Keyboard arrows select actors when the scene
   has focus; `+`, `-`, and `0` control the camera.
3. **Resume the fabricator**. The backlog drains through the same channels.
4. **Close Aster bay**. It finishes its current cargo; Boreal takes arrivals.
   Reopen it, then **trip a crane controller**. CSP supervision restarts Aster
   while preserving its accepted cargo.
5. **Evacuate the dock**. Arrival control stops admitting work, paused actors
   resume, and endpoint closure propagates downstream. The completion screen
   appears only after all accepted cargo is delivered and all six actors exit.
   Start another shift without restarting the server.

## What is real

Six independently scheduled actors use typed CSP channels:

```text
                      ┌─ Aster ─┐
Arrival ── [8] ────────┤        ├── [10] ── Warehouse ── [0] ── Fabricator ── [4] ── Tram
                      └─ Boreal ┘
```

Brackets show buffer capacity. A zero-capacity channel requires a rendezvous.
The source and five downstream workers together can hold six more cargo,
giving an upper bound of 28 admitted, undelivered cargo. Each cargo completes
four channel sends before delivery. Aster's durable cargo slot belongs to its
supervisor scope, so a restarted invocation keeps its work.

A coordinator owns the observation ledger and consumes lossless worker
events. It does not schedule cargo or choose a crane. Successful sends are
reported after they commit; out-of-order sender and receiver notifications
cannot rewind cargo to an earlier stage. Controls remain selectable while
workers wait on full channels. Full control mailboxes reject commands instead
of blocking telemetry. Evacuation retries pending notifications while events
continue to drain.

HTTP handlers request snapshots through capacity-one reply mailboxes. A slow
or disconnected viewer cannot hold the coordinator waiting for a reply read.
The canvas interpolates within the last reported stage; it cannot create,
route, or deliver cargo. Ship shapes, cargo colors, buildings and lights are
illustrative. Stage counts are observed cargo populations, not exact buffer
occupancy; a worker may hold an item from that stage.

This is the first dock, not a full city or a deterministic replay engine.
Scheduling and wall-clock timings vary. The six actors exclude HTTP handlers,
the observation coordinator and CSP's runtime support processes.

## Verification

```sh
make test-lifeboat
uv run --with playwright==1.60.0 playwright install chromium
make journey-lifeboat
```

The self-test exercises this same distributed-library consumer with one and
four runtime workers: flow, congestion, recovery, an injected controller
failure, an abandoned viewer, evacuation, and a new shift. It checks cargo
conservation, bounded population, exactly four completed sends per delivered
cargo, one supervisor restart and complete actor shutdown.

The Playwright journey starts the actual server and operates the rendered UI.
It checks the same conservation relationships, control effects, keyboard
inspection, responsive geometry, evacuation and restart. Screenshots and logs
go to a temporary directory; `--artifacts PATH` selects another scratch path.
The `lifeboat` CI job runs both live self-tests and this browser journey.

`formal/Lifeboat.tla`, included in `make check`, explores a three-cargo,
capacity-one routing abstraction. TLC checks unique ownership, conservation,
bounded stages and eventual drain after admissions stop under weak fairness.
It does not prove the C++ implementation, controller recovery, event ordering,
or the memory model. The live tests cover those observable scenarios; visual
clarity and whether the experience feels compelling remain human judgments.
