# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright==1.60.0"]
# ///
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Exercise the built demo, live CSP backend and real browser as one product.

Screenshots/logs go to a temporary directory by default, never the checkout.
Run: uv run demos/lifeboat/journey.py [--artifacts /private/tmp/lifeboat-journey]
"""
from __future__ import annotations
import argparse
import copy
import json
import math
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.error import URLError
from urllib.request import urlopen
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[2]

# These waypoints are the product contract, deliberately independent of the
# backend's phase catalog. Instantaneous bookkeeping phases may be coalesced.
CARGO_ROUTE = (
    "crane-lift", "crane-slew", "crane-lower", "road-to-hold",
    "hold-intake", "warehouse-out", "to-fabricator", "fab-out",
    "platform", "loading", "tram-ride", "habitat-unload",
)
# Surveyed drawing landmarks: a mislabeled phase must not let a shortcut pass.
# The platform has several reserved slots, so only its shared y/z are fixed.
LANDMARKS = {
    "road-to-hold": (-50, -180, 8),
    "warehouse-out": (-49, -12, 8),
    "to-fabricator": (175, -23, 8),
    "fab-out": (284, -10, 8),
    "tram-ride": (398, 283, 33),
    "habitat-unload": (430, 237, 50),
}
POSITION_EPSILON = 0.15  # Four-decimal wire timestamps round fast motions.
SAMPLE_INTERVAL = 0.1
LOGISTICS_HANDOFFS = 4  # Arrival → cranes → hold → fabricator → tram.
HOLD_BUFFER_CAPACITY = 10


def position(motion: dict, now: float) -> tuple[float, ...]:
    """Independent implementation of the documented smoothstep wire format."""
    frames = motion["frames"]
    t = min(1.0, max(0.0, (now - motion["at"]) / motion["duration"])) if motion["duration"] else 1.0
    for a, b in zip(frames, frames[1:]):
        if t <= b[0]:
            fraction = max(0.0, (t - a[0]) / (b[0] - a[0]))
            eased = fraction * fraction * (3 - 2 * fraction)
            return tuple(x + (y - x) * eased for x, y in zip(a[1:], b[1:]))
    return tuple(frames[-1][1:])


def max_speed(motion: dict) -> float:
    if not motion["duration"]:
        return 0.0
    # Smoothstep's derivative peaks at 1.5, so this is a mathematical upper
    # bound, not a threshold tuned to make this implementation pass.
    return max((1.5 * math.dist(a[1:], b[1:]) / ((b[0] - a[0]) * motion["duration"])
                for a, b in zip(motion["frames"], motion["frames"][1:])), default=0.0)


class SceneOracle:
    """Verify the real wire history without consulting the simulation ledger."""

    def __init__(self) -> None:
        self.run: int | None = None
        self.seq = 0
        self.now = 0.0
        self.tracks: dict[str, dict] = {}
        self.history: dict[str, list[dict]] = {}
        self.phases: dict[int, list[str]] = {}
        self.removed: set[str] = set()
        self.messages = 0
        self.revisions = 0
        self.state: dict = {}
        self.kinds: set[str] = set()
        self.completed = 0

    def accept(self, scene: dict) -> None:
        assert scene["type"] == "scene", scene
        assert scene["seq"] == self.seq + 1, (self.seq, scene["seq"])
        self.seq = scene["seq"]
        self.messages += 1
        if scene["run"] != self.run:
            assert scene["reset"] and "state" in scene, "new run needs a full snapshot"
            self.run = scene["run"]
            self.tracks.clear()
            self.history.clear()
            self.phases.clear()
            self.removed.clear()
            self.now = 0.0
        assert scene["now"] >= self.now, "scene time went backwards"
        self.now = scene["now"]
        motions = scene["motions"]
        assert len({m["key"] for m in motions}) == len(motions), "duplicate logical channel"
        for motion in motions:
            key = motion["key"]
            assert key not in self.removed, f"removed entity reappeared: {key}"
            assert motion["duration"] >= 0 and motion["at"] >= 0, motion
            assert isinstance(motion["revision"], int) and motion["revision"] > 0, motion
            frames = motion["frames"]
            assert frames and all(len(frame) == 4 and all(math.isfinite(v) for v in frame) for frame in frames), motion
            assert frames[0][0] == 0 and (len(frames) == 1 or frames[-1][0] == 1), motion
            assert all(a[0] < b[0] for a, b in zip(frames, frames[1:])), motion
            previous = self.tracks.get(key)
            if previous:
                assert motion["revision"] >= previous["revision"], f"revision rewound: {key}"
                if key.startswith("cargo:"):
                    assert motion["cargo"] == previous["cargo"], f"entity changed cargo identity: {key}"
                if motion["revision"] == previous["revision"]:
                    assert motion == previous, f"same revision changed: {key}"
                    continue
                assert motion["at"] >= previous["at"], f"motion time rewound: {key}"
                # A fresh command must start where the previous command puts
                # this same entity, including commands interrupted mid-motion.
                gap = math.dist(position(previous, motion["at"]), frames[0][1:])
                assert gap <= POSITION_EPSILON, f"teleport on {key}: {previous['phase']} -> {motion['phase']}, gap {gap:.3f}"
            if key.startswith("cargo:"):
                assert key == f"cargo:{motion['cargo']}", f"unstable cargo channel: {key}"
                if motion["kind"] == "hidden":
                    assert motion["phase"] in ("stored", "processing", "conversion"), "cargo concealed outside storage or fabrication"
                    assert all(frame[3] == 8 for frame in frames), "cargo concealed in midair"
                if motion["phase"] in ("fab-out", "conveyor", "platform", "loading", "tram-ride", "habitat-unload"):
                    assert motion["kind"] == "goods", "fabrication did not produce finished goods on the same cargo channel"
                if motion["duration"] > 0:
                    destination = frames[-1][1:]
                    if motion["phase"] in LANDMARKS:
                        assert math.dist(destination, LANDMARKS[motion["phase"]]) <= POSITION_EPSILON, f"wrong physical waypoint: {key} {motion['phase']} {destination}"
                    if motion["phase"] in ("crane-lift", "crane-slew"):
                        assert destination[2] == 112, "cargo did not follow the crane's lifted path"
                    if motion["phase"] == "crane-lower":
                        assert destination[2] == 8, "cargo did not reach the road surface"
                    if motion["phase"] == "road-to-hold":
                        assert all(frame[3] == 8 for frame in frames), "road cargo moved through the air"
                    if motion["phase"] == "platform":
                        assert destination[1:] == [228, 12] and 111 <= destination[0] <= 231, "finished goods missed the platform"
                phases = self.phases.setdefault(motion["cargo"], [])
                if not phases or phases[-1] != motion["phase"]:
                    phases.append(motion["phase"])
            self.kinds.add(key.split(":", 1)[0])
            self.tracks[key] = copy.deepcopy(motion)
            self.history.setdefault(key, []).append(copy.deepcopy(motion))
            self.revisions += 1
        for removal in scene["removed"]:
            key = removal["key"]
            previous = self.tracks.pop(key, None)
            assert previous is not None, f"unknown entity removed: {key}"
            assert removal["at"] + 0.001 >= previous["at"] + previous["duration"], f"mid-motion removal: {key}"
            if key.startswith("cargo:"):
                assert previous["phase"] == "habitat-unload", f"cargo vanished before reaching the habitat: {key}"
            self.removed.add(key)
        if "state" in scene:
            self.state = scene["state"]
            assert self.state["violations"] == 0, self.state
            assert self.state["motionViolations"] == 0, self.state
            assert self.state["created"] == self.state["delivered"] + self.state["inFlight"], self.state
            assert len({item["id"] for item in self.state["cargo"]}) == len(self.state["cargo"]), "duplicate ledger cargo"
            self.completed = max(self.completed, self.state["motionCompleted"])

    def require_route(self, cargo: int) -> None:
        phases = self.phases.get(cargo, [])
        cursor = 0
        for phase in phases:
            if cursor < len(CARGO_ROUTE) and phase == CARGO_ROUTE[cursor]:
                cursor += 1
        assert cursor == len(CARGO_ROUTE), f"cargo {cargo} missed {CARGO_ROUTE[cursor:]}; observed {phases}"
        assert f"cargo:{cargo}" in self.removed, f"cargo {cargo} did not finish its physical delivery"


class RenderOracle:
    def __init__(self) -> None:
        self.run: int | None = None
        self.previous: dict = {}
        self.at = 0.0
        self.samples = 0
        self.moving_samples = 0
        self.checked_kinds: set[str] = set()
        self.max_gap = 0.0

    def accept(self, render: dict, wire: SceneOracle) -> None:
        poses, now = render["poses"], render["at"]
        if render["run"] != self.run:
            self.run, self.previous, self.at = render["run"], {}, now
        assert now >= self.at, "render time rewound"
        for key, pose in poses.items():
            history = wire.history.get(key, [])
            matching = next((m for m in reversed(history) if m["revision"] == pose["revision"]), None)
            assert matching is not None, f"canvas drew an unknown logical channel/revision: {key}"
            actual = tuple(pose[axis] for axis in ("x", "y", "z"))
            gap = math.dist(actual, position(matching, now))
            self.max_gap = max(self.max_gap, gap)
            assert gap <= POSITION_EPSILON, f"canvas departed from server-authored motion: {key}, gap {gap:.3f}"
            self.checked_kinds.add(key.split(":", 1)[0])
            if key in self.previous and now > self.at:
                old = tuple(self.previous[key][axis] for axis in ("x", "y", "z"))
                distance = math.dist(old, actual)
                active = [m for m in history if m["at"] <= now and m["at"] + m["duration"] >= self.at]
                speed = max((max_speed(m) for m in active), default=0.0)
                assert distance <= speed * (now - self.at) + 2 * POSITION_EPSILON, f"rendered teleport on {key}: {distance:.3f} units in {now - self.at:.3f}s"
                self.moving_samples += distance > POSITION_EPSILON
        self.previous, self.at = poses, now
        self.samples += 1


def teleport_probe(browser, origin: str, artifacts: Path) -> dict:
    """Corrupt a real second viewer, preserving every telemetry field.

    The normal journey already passed before this probe. The proxy changes
    only one moving entity's geometry. Both the wire boundary and actual draw
    sampling must reject it; a synthetic scene never contributes live proof.
    """
    context = browser.new_context(viewport={"width": 1500, "height": 1100})
    page = context.new_page()
    wire = SceneOracle()
    render = RenderOracle()
    injection: dict = {}
    wire_rejection = ""
    render_rejection = ""
    visible: set[str] = set()

    def intercept(route) -> None:
        upstream = route.connect_to_server()

        def received(payload) -> None:
            nonlocal wire_rejection
            scene = json.loads(payload)
            original_state = copy.deepcopy(scene.get("state"))
            if not injection:
                for motion in scene["motions"]:
                    if motion["key"] in visible and motion["key"].startswith("cargo:") and max_speed(motion) > 0:
                        previous = wire.tracks.get(motion["key"])
                        if previous and previous["revision"] < motion["revision"]:
                            injection.update(key=motion["key"], revision=motion["revision"], phase=motion["phase"], translation=1200)
                            for frame in motion["frames"]:
                                frame[1] += injection["translation"]
                            break
            assert scene.get("state") == original_state, "negative probe changed telemetry"
            try:
                wire.accept(scene)
            except AssertionError as error:
                if not injection:
                    raise
                wire_rejection = wire_rejection or str(error)
                # Record exactly what the browser receives for the second,
                # independent render oracle even though the wire oracle has
                # correctly rejected this intentionally corrupt command.
                for motion in scene["motions"]:
                    history = wire.history.setdefault(motion["key"], [])
                    if not history or history[-1]["revision"] < motion["revision"]:
                        history.append(copy.deepcopy(motion))
                        wire.tracks[motion["key"]] = copy.deepcopy(motion)
            route.send(json.dumps(scene))

        upstream.on_message(received)

    page.route_web_socket("**/api/stream", intercept)
    page.goto(origin, wait_until="domcontentloaded")
    deadline = time.monotonic() + 35
    try:
        while not render_rejection:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"live teleport was not rejected by both checks: {injection}, wire={wire_rejection!r}")
            page.wait_for_timeout(SAMPLE_INTERVAL * 1000)
            actual = page.evaluate("""() => window.lifeboat?.state && ({
              poses:window.lifeboat.poses,at:window.lifeboat.renderedAt,run:window.lifeboat.run})""")
            if not actual or not actual.get("poses"):
                continue
            visible = {key for key, pose in actual["poses"].items() if pose["visible"]}
            try:
                render.accept(actual, wire)
            except AssertionError as error:
                if not injection:
                    raise
                render_rejection = str(error)
        assert "teleport" in wire_rejection, wire_rejection
        assert "rendered teleport" in render_rejection, render_rejection
        state = page.evaluate("window.lifeboat.state")
        assert state["violations"] == 0 and state["created"] == state["delivered"] + state["inFlight"], state
        page.screenshot(path=str(artifacts / "teleport-negative-control.png"), full_page=True)
        return {**injection, "wireRejected": wire_rejection, "renderRejected": render_rejection, "telemetryHealthy": True}
    finally:
        context.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--project-root", type=Path, default=ROOT)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="lifeboat-journey-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    origin = f"http://127.0.0.1:{port}"
    with (artifacts / "server.log").open("w") as log:
        project = args.project_root.resolve()
        binary = (args.binary or project / "build/lifeboat/lifeboat").resolve()
        server = subprocess.Popen([str(binary), "--port", str(port)], cwd=project, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 20
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f"server exited {server.returncode}; see {artifacts / 'server.log'}")
                try:
                    with urlopen(origin + "/api/state", timeout=1) as response:
                        json.load(response)
                    break
                except (URLError, TimeoutError):
                    if time.monotonic() > deadline:
                        raise TimeoutError("live server did not start")
                    time.sleep(0.05)
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch()
                page = browser.new_page(viewport={"width": 1500, "height": 1100}, device_scale_factor=1)
                errors: list[str] = []
                wire = SceneOracle()
                rendered = RenderOracle()
                wire_failures: list[str] = []
                streams: list = []
                acknowledgments: list[int] = []
                polls: list[str] = []
                traffic = {"bytes": 0, "frames": 0, "peakFrameBytes": 0, "firstAt": None, "lastAt": None}
                page.on("request", lambda request: polls.append(request.url) if request.method == "GET" and "/api/state" in request.url else None)

                def stream_opened(stream) -> None:
                    streams.append(stream)
                    if len(streams) > 1:
                        return  # Extra viewers below are tested separately.
                    assert stream.url == origin.replace("http:", "ws:") + "/api/stream", stream.url

                    def received(payload) -> None:
                        if wire_failures:
                            return
                        size = len(payload.encode("utf-8")) if isinstance(payload, str) else len(payload)
                        traffic["bytes"] += size
                        traffic["frames"] += 1
                        traffic["peakFrameBytes"] = max(traffic["peakFrameBytes"], size)
                        traffic["lastAt"] = time.monotonic()
                        if traffic["firstAt"] is None:
                            traffic["firstAt"] = traffic["lastAt"]
                        scene = None
                        try:
                            scene = json.loads(payload)
                            wire.accept(scene)
                        except (AssertionError, KeyError, TypeError, ValueError) as error:
                            wire_failures.append(str(error))
                            (artifacts / "wire-failure.json").write_text(json.dumps({"error": str(error), "scene": scene, "previousTracks": wire.tracks}, indent=2) + "\n")

                    stream.on("framereceived", received)
                    stream.on("framesent", lambda payload: acknowledgments.append(int(payload)))

                page.on("websocket", stream_opened)
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.on("console", lambda message: errors.append(message.text) if message.type == "error" else None)
                page.goto(origin, wait_until="domcontentloaded")

                def sample() -> None:
                    page.wait_for_timeout(SAMPLE_INTERVAL * 1000)
                    actual = page.evaluate("""() => window.lifeboat?.state && ({
                      poses:window.lifeboat.poses, at:window.lifeboat.renderedAt,
                      run:window.lifeboat.run})""")
                    assert not wire_failures, wire_failures
                    assert not polls, f"frontend polled HTTP instead of streaming: {polls}"
                    if actual and actual.get("poses") and wire.run == actual["run"]:
                        try:
                            rendered.accept(actual, wire)
                        except AssertionError as error:
                            (artifacts / "render-failure.json").write_text(json.dumps({"error": str(error), "render": actual, "previous": rendered.previous, "previousAt": rendered.at, "history": wire.history}, indent=2) + "\n")
                            raise

                def wait_state(condition: str, timeout: int = 120000) -> dict:
                    deadline = time.monotonic() + timeout / 1000
                    while not page.evaluate(f"() => Boolean(window.lifeboat?.state && ({condition}))"):
                        if time.monotonic() > deadline:
                            raise TimeoutError(f"state condition timed out: {condition}; {page.evaluate('window.lifeboat?.state')}")
                        sample()
                    state = page.evaluate("window.lifeboat.state")
                    assert state["violations"] == 0, state
                    assert state["created"] == state["delivered"] + state["inFlight"], state
                    assert len({item["id"] for item in state["cargo"]}) == len(state["cargo"])
                    return state

                def wait_browser(condition: str, timeout: float = 10) -> None:
                    deadline = time.monotonic() + timeout
                    while not page.evaluate(f"() => Boolean({condition})"):
                        if time.monotonic() >= deadline:
                            raise TimeoutError(f"browser condition timed out: {condition}")
                        sample()

                def run_for(seconds: float) -> None:
                    deadline = time.monotonic() + seconds
                    while time.monotonic() < deadline:
                        sample()

                def viewer(name: str) -> None:
                    page.evaluate("""([url,name]) => {
                      window.__journeyViewers ||= {};
                      const v = {frames:[],closed:false};
                      const ws = new WebSocket(url); v.ws=ws;
                      window.__journeyViewers[name]=v;
                      ws.onmessage=e=>{if(v.frames.length<4)v.frames.push(JSON.parse(e.data));else v.overflow=true;};
                      ws.onclose=()=>v.closed=true;
                    }""", [origin.replace("http:", "ws:") + "/api/stream", name])
                    wait_browser(f"window.__journeyViewers.{name}.frames.length >= 1")

                def transport() -> dict:
                    viewer("slow")
                    before = wire.now
                    before_revisions = wire.revisions
                    run_for(2)
                    slow = page.evaluate("({frames:window.__journeyViewers.slow.frames,closed:window.__journeyViewers.slow.closed})")
                    assert len(slow["frames"]) == 1 and not slow["closed"], "unacked viewer received more than its one-frame window"
                    assert wire.now >= before + 1.5 and wire.revisions > before_revisions, "unacked viewer blocked simulation motion"
                    first = slow["frames"][0]
                    assert first["reset"] and first["seq"] == 1 and "state" in first
                    page.evaluate("window.__journeyViewers.slow.ws.send(String(window.__journeyViewers.slow.frames[0].seq))")
                    wait_browser("window.__journeyViewers.slow.frames.length >= 2")
                    second = page.evaluate("window.__journeyViewers.slow.frames[1]")
                    assert second["seq"] == 2 and second["now"] >= first["now"] + 1.5, "ACK replayed backlog instead of coalescing current scene"
                    page.evaluate("window.__journeyViewers.slow.ws.close()")
                    viewer("reconnect")
                    full = page.evaluate("window.__journeyViewers.reconnect.frames[0]")
                    assert full["reset"] and full["seq"] == 1 and "state" in full, "reconnect was not a full snapshot"
                    fresh = SceneOracle()
                    fresh.accept(full)
                    assert {"crane:1", "crane:2", "door:hold", "door:hold-in", "door:fab-in", "door:fab-out", "tram:5"} <= fresh.tracks.keys(), "reconnect omitted durable scene entities"
                    assert all(f"cargo:{item['id']}" in fresh.tracks for item in full["state"]["cargo"]), "reconnect omitted live cargo identities"
                    # Intentionally abandon this viewer. It must be evicted
                    # while the main renderer continues advancing normally.
                    before = wire.now
                    wait_browser("window.__journeyViewers.reconnect.closed", timeout=8)
                    assert wire.now >= before + 3, "abandoned viewer stalled the live scene"
                    return {"unackedFrames": len(slow["frames"]), "coalescedSeconds": second["now"] - first["now"], "reconnectTracks": len(full["motions"]), "abandonedViewerClosed": True}

                def layout() -> None:
                    # ResizeObserver and painting run after a viewport change.
                    # Check the real backing surface before taking evidence.
                    deadline = time.monotonic() + 5
                    previous_draw = page.evaluate("window.lifeboat.renderedAt")
                    while not page.evaluate("""() => {
                      const c=document.querySelector('#world'),r=c.getBoundingClientRect(),d=Math.min(devicePixelRatio,2);
                      return Math.abs(c.width-r.width*d)<=2 && Math.abs(c.height-r.height*d)<=2;
                    }"""):
                        if time.monotonic() > deadline:
                            raise TimeoutError("canvas backing size did not match its displayed size")
                        time.sleep(0.05)
                    wait_browser(f"window.lifeboat.renderedAt > {previous_draw}", timeout=5)
                    geometry = page.evaluate("""() => {
                      const box = s => {const r=document.querySelector(s).getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom,width:r.width,height:r.height}};
                      return {width:innerWidth,scroll:document.documentElement.scrollWidth,scene:box('.viewport'),canvas:box('#world'),side:box('.sidebar'),overflow:[...document.querySelectorAll('button,h1,h3,.connection')].filter(e=>e.offsetWidth && e.scrollWidth>e.clientWidth+2).map(e=>e.textContent)};
                    }""")
                    assert geometry["scroll"] <= geometry["width"], geometry
                    assert not geometry["overflow"], geometry
                    scene, side = geometry["scene"], geometry["side"]
                    assert 400 <= scene["height"] <= 1000, geometry
                    assert abs(geometry["canvas"]["height"] - scene["height"]) <= 2, geometry
                    if geometry["width"] > 800:
                        assert side["x"] >= scene["right"] + 8, geometry
                    else:
                        assert side["y"] >= scene["bottom"] + 8, geometry

                print("Checking the first cargo's complete physical route…", flush=True)
                wait_state("window.lifeboat.state.delivered >= 1")
                # Delivery telemetry alone is insufficient: cargo #1 must have
                # traversed the complete physical route on its original key.
                run_for(0.2)
                wire.require_route(1)
                route = wire.phases[1][:]
                print("Checking WebSocket backpressure, catchup, reconnect and timeout…", flush=True)
                transport_result = transport()
                layout()
                page.locator("#xray").click()
                page.locator("#world").focus()
                page.keyboard.press("ArrowRight")
                assert page.locator("#inspectorTitle").inner_text() == "Arrival control"
                page.locator("#zoomIn").click()
                page.locator("#resetView").click()
                assert page.locator("#xray").get_attribute("aria-pressed") == "true"
                page.locator("#xray").click()
                page.screenshot(path=str(artifacts / "desktop.png"), full_page=True)
                print("Checking congestion and control selection while cargo is blocked…", flush=True)
                page.locator('[data-command="bay"]').click()
                wait_state("window.lifeboat.state.bayClosed")
                page.locator('[data-command="factory"]').click()
                wait_state("window.lifeboat.state.factoryPaused")
                page.locator('[data-command="surge"]').click()
                congested = wait_state("window.lifeboat.state.inFlight >= 15")
                page.locator("#xray").click()
                page.screenshot(path=str(artifacts / "congestion.png"), full_page=True)
                # Force the exact move-only ownership hazard: the admission
                # channel is full while the source owns its next cargo. A
                # competing control must leave those Entity endpoints intact
                # until the send branch is eventually selected.
                blocked = wait_state(f'window.lifeboat.state.actors[0].status === "backpressure" && window.lifeboat.state.cargo.filter(c => c.stage === "stored").length >= {HOLD_BUFFER_CAPACITY + 1}')
                held_cargo = blocked["actors"][0]["cargo"]
                assert held_cargo > 0
                page.locator('[data-command="surge"]').click()
                wait_state("!window.lifeboat.state.surge")
                run_for(0.3)
                assert page.evaluate("window.lifeboat.state.actors[0].cargo") == held_cargo, "control discarded cargo waiting on a full channel"
                page.locator('[data-command="factory"]').click()
                page.locator('[data-command="bay"]').click()
                page.locator('[data-command="fault"]').click()
                wait_state(f"window.lifeboat.state.restarts >= 1 && window.lifeboat.state.delivered >= {congested['delivered'] + 3}")
                page.set_viewport_size({"width": 390, "height": 844})
                layout()
                page.screenshot(path=str(artifacts / "mobile.png"), full_page=True)
                print("Checking complete physical evacuation and imp shutdown…", flush=True)
                page.locator('[data-command="evacuate"]').click()
                drained = wait_state('window.lifeboat.state.mode === "evacuated"')
                assert drained["activeActors"] == 0 and drained["inFlight"] == 0, drained
                assert drained["created"] == drained["delivered"], drained
                assert f"cargo:{held_cargo}" in wire.removed, "control-interrupted cargo never completed delivery"
                assert drained["transfers"] == drained["created"] * LOGISTICS_HANDOFFS, "a cargo skipped or duplicated a logistics channel handoff"
                assert drained["motionCompleted"] >= drained["created"] * len(CARGO_ROUTE), "entity completion channels did not finish the required physical steps"
                assert all(actor["status"] == "offline" for actor in drained["actors"]), drained
                assert drained["animationActors"] == 0, "entity imps survived complete drain"
                assert page.locator("#evacuated").is_visible()
                page.set_viewport_size({"width": 1500, "height": 1100})
                page.screenshot(path=str(artifacts / "evacuated.png"), full_page=True)
                page.locator('[data-command="reset"]').click()
                wait_state(f"window.lifeboat.state.run === {drained['run'] + 1} && window.lifeboat.state.delivered >= 1")
                assert not page.locator("#evacuated").is_visible()
                assert not errors, errors
                assert not wire_failures and not polls, (wire_failures, polls)
                assert wire.messages > 20 and acknowledgments, "no real acknowledged scene stream"
                assert wire.completed > len(CARGO_ROUTE), "motion completion channels did not report completed work"
                assert acknowledgments == list(range(1, len(acknowledgments) + 1)), "ACKs did not match transport sequence"
                assert {"cargo", "crane", "door", "tram"} <= rendered.checked_kinds, rendered.checked_kinds
                assert rendered.samples >= 100 and rendered.moving_samples >= 100, "insufficient actual moving render samples"
                print("Checking a real geometry-only teleport negative control…", flush=True)
                negative_control = teleport_probe(browser, origin, artifacts)
                browser.close()
                elapsed = traffic.pop("lastAt") - traffic.pop("firstAt")
                traffic.update(elapsedSeconds=elapsed, kibibytesPerSecond=traffic["bytes"] / 1024 / elapsed)
                (artifacts / "result.json").write_text(json.dumps({"passed": True, "cargo1Route": route, "controlInterruptedCargo": held_cargo, "transport": transport_result, "traffic": traffic, "negativeControl": negative_control, "renderSamples": rendered.samples, "movingSamples": rendered.moving_samples, "maxRenderGap": rendered.max_gap, "sceneFrames": wire.messages, "motionRevisions": wire.revisions, "evacuated": drained}, indent=2) + "\n")
                print(f"Main viewer traffic: {traffic['frames']} frames, {traffic['bytes']} bytes, peak {traffic['peakFrameBytes']} bytes, {elapsed:.1f}s, {traffic['kibibytesPerSecond']:.2f} KiB/s.")
                print(f"PASS: live cargo choreography, WebSocket backpressure/reconnect, UI congestion/recovery, evacuation/restart and responsive layout. Artifacts: {artifacts}")
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
