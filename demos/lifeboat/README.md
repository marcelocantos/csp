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
and HTTP/1.1 and WebSocket files with the in-tree llhttp and wslay dependencies.

Use `--port 8043` for another port, or `--assets PATH` when running outside
the repository root. `--help-agent` describes the HTTP controls. The server
binds to loopback by default. Stop it with Ctrl-C.

## Try this sequence

1. **Pause the fabricator** and **call a traffic surge**. Storage fills;
   cranes wait to hand over cargo; eventually arrival control waits too.
2. Enable **Channel view** to see the topology and actual stage populations.
   Select a structure to inspect its live status. Drag, scroll, or use the
   zoom controls to explore. Keyboard arrows select structures when the scene
   has focus; `+`, `-`, and `0` control the camera.
3. **Resume the fabricator**. The backlog drains through the same channels.
4. **Close Aster bay**. It finishes its current cargo; Boreal takes arrivals.
   Reopen it, then **trip a crane controller**. CSP supervision restarts Aster
   while preserving its accepted cargo.
5. **Evacuate the dock**. Arrival control stops admitting work, paused imps
   resume, and endpoint closure propagates downstream. The completion screen
   appears only after all accepted cargo is delivered and every logistics and
   motion imp exits.
   Start another shift without restarting the server.

## What is real

Six independently scheduled logistics imps use typed CSP channels:

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

Every crane rig, ship, box, cargo door, fabricator effect and Meridian carrier
also has its own imp and typed motion-command inbox. Each imp owns its pose,
executes timed waypoints and acknowledges completion. Logistics imps wait for
those acknowledgements before making the next physical handoff. A crane fault
freezes its twins at their current poses; supervision resumes the unfinished
movement without replaying completed steps. These are conventional CSP imps,
with no AI control.

Select a box to follow its ID. It arrives on a ship, is lifted onto the powered
receiving lane, travels around the hold and enters through the rear gate.
The warehouse door releases that same box to the fabricator. Inside, raw cargo
becomes finished supplies with gold shipping bands. A visible output conveyor
feeds Meridian's platform; the carrier delivers the box into the habitat and
returns empty. Storage and platform slots are bounded channel resources.

The browser receives timed motion revisions over one shared WebSocket. It
interpolates generic waypoints at display refresh rate with an 80 ms playback
delay; it never chooses
a route, advances the choreography, or reports physical completion. Decorative
stars and indicator lights are illustrative. Stage counts are observed cargo
populations, not exact buffer occupancy; an imp may hold an item from that stage.

Each viewer has at most one unacknowledged frame. Until it acknowledges that
frame, the server requests no further snapshots for it and queues no scene
updates. The next frame catches up to the current scene. A five-second timeout
hard-closes both socket imps, and reconnecting starts with a complete snapshot.
Snapshot replies use capacity-one mailboxes, so abandoned viewers cannot block
the observation coordinator.

This is the first dock, not a full city or a deterministic replay engine.
Scheduling and wall-clock timings vary. The imp counters distinguish the six
logistics imps from the changing population of motion imps; both exclude HTTP
handlers, the observation coordinator and CSP's runtime support processes.

## Scene protocol

`GET /api/stream` upgrades to a WebSocket. Each text frame contains
`type: "scene"`, a connection-local `seq`, station clock `now`, shift `run`, `reset`,
changed `motions`, and timestamped `removed` entries. Telemetry appears in
`state` at most every 200 ms. The first frame and every new shift carry the
complete scene. The client acknowledges with the decimal sequence alone, such
as `"12"`; messages are limited to 20 bytes. Invalid acknowledgements close the
connection.

A motion identifies its visual channel by `key` (for example `cargo:17` or
`crane:1`) and carries `kind`, `phase`, `cargo`, `actor`, `revision`, start `at`,
`duration`, and `frames: [[t,x,y,z], ...]`. Normalized keyframe times run from
zero to one, with smoothstep interpolation per segment. Doors and effects use
the x coordinate as an opening or intensity value. The logical channels share
one transport; there is no socket per imp and no per-pixel stream.

Snapshots are sampled at most every 50 ms. Only changed motion revisions are
sent, with clock heartbeats every 200 ms while movement is active. The browser
keeps bounded recent revisions to render across message arrival boundaries,
applies removals on the same delayed clock, and freezes on disconnection.

Measured on macOS with the default local server and Chromium on 2026-09-13:

| Scenario | Duration | Scene frames | JSON payload | Mean payload rate | Largest frame |
|---|---:|---:|---:|---:|---:|
| Normal arrivals | 29.83 s | 279 | 275,500 bytes | 9.02 KiB/s | 3,756 bytes |
| Full congestion/recovery journey | 167.05 s | 1,181 | 1,767,996 bytes | 10.34 KiB/s | 5,203 bytes |

These are observations from the live stream, excluding WebSocket/TCP framing,
not performance thresholds. The normal run sent 729 bytes of acknowledgement
payload in the reverse direction. The browser journey writes fresh traffic
measurements to its `result.json` on each run.

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
cargo, one supervisor restart, continuous motion endpoints and complete imp
shutdown.

The Playwright journey starts the actual server and operates the rendered UI.
It checks each phase and physical waypoint of a complete cargo journey,
compares sampled canvas poses with the real motion stream, and exercises
control selection while a cargo with move-only imp endpoints is blocked.
It verifies slow-viewer coalescing, reconnect, timeout, responsive geometry,
evacuation and restart. A second viewer receives an intentionally translated
motion with unchanged telemetry: both wire and render checks must reject this
teleport. Screenshots and logs go to a temporary directory; `--artifacts PATH`
selects another scratch path. The `lifeboat` CI job runs both live self-tests
and this browser journey.

`formal/Lifeboat.tla`, included in `make check`, explores a three-cargo,
capacity-one routing abstraction. TLC checks unique ownership, conservation,
bounded stages and eventual drain after admissions stop under weak fairness.
It does not prove the C++ implementation, controller recovery, event ordering,
or the memory model. The live tests cover those observable scenarios; visual
clarity and whether the experience feels compelling remain human judgments.

`CargoSelection.tla` models retaining cargo ownership when a competing control
wins a channel selection. `WebSocketClose.tla` models cancellation and joining
both I/O imps before releasing the socket. Fixed models run in `make check`;
their `_Bug` companions expose the eager move, early socket release and
undrained completion failures. Real-socket WebSocket tests cover blocked
reads, writes and channel forwarding, plus message and fragmentation limits.

The same local journey delivered all 28 accepted cargo through 112 logistics
handoffs, completed 1,148 motion commands and left no logistics or motion imps.
It checked 1,381 rendered samples, including 5,496 moving-pose comparisons,
with zero pose discrepancy. These runs validate the local macOS path;
Linux and Windows socket behavior still need their platform runs.
