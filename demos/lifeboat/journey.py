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
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.error import URLError
from urllib.request import urlopen
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="lifeboat-journey-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    origin = f"http://127.0.0.1:{port}"
    with (artifacts / "server.log").open("w") as log:
        server = subprocess.Popen([str(ROOT / "build/lifeboat/lifeboat"), "--port", str(port)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
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
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.on("console", lambda message: errors.append(message.text) if message.type == "error" else None)
                page.goto(origin, wait_until="domcontentloaded")

                def wait_state(condition: str, timeout: int = 45000) -> dict:
                    deadline = time.monotonic() + timeout / 1000
                    while not page.evaluate(f"() => Boolean(window.lifeboat?.state && ({condition}))"):
                        if time.monotonic() > deadline:
                            raise TimeoutError(f"state condition timed out: {condition}; {page.evaluate('window.lifeboat?.state')}")
                        time.sleep(0.1)
                    state = page.evaluate("window.lifeboat.state")
                    assert state["violations"] == 0, state
                    assert state["created"] == state["delivered"] + state["inFlight"], state
                    assert len({item["id"] for item in state["cargo"]}) == len(state["cargo"])
                    return state

                def layout() -> None:
                    # ResizeObserver and painting run after a viewport change.
                    # Check the real backing surface before taking evidence.
                    deadline = time.monotonic() + 5
                    while not page.evaluate("""() => {
                      const c=document.querySelector('#world'),r=c.getBoundingClientRect(),d=Math.min(devicePixelRatio,2);
                      return Math.abs(c.width-r.width*d)<=2 && Math.abs(c.height-r.height*d)<=2;
                    }"""):
                        if time.monotonic() > deadline:
                            raise TimeoutError("canvas backing size did not match its displayed size")
                        time.sleep(0.05)
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

                wait_state("window.lifeboat.state.delivered >= 3")
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
                page.locator('[data-command="bay"]').click()
                wait_state("window.lifeboat.state.bayClosed")
                page.locator('[data-command="factory"]').click()
                wait_state("window.lifeboat.state.factoryPaused")
                page.locator('[data-command="surge"]').click()
                congested = wait_state("window.lifeboat.state.inFlight >= 15")
                page.locator("#xray").click()
                page.screenshot(path=str(artifacts / "congestion.png"), full_page=True)
                page.locator('[data-command="factory"]').click()
                page.locator('[data-command="bay"]').click()
                page.locator('[data-command="fault"]').click()
                wait_state(f"window.lifeboat.state.restarts >= 1 && window.lifeboat.state.delivered >= {congested['delivered'] + 3}")
                page.set_viewport_size({"width": 390, "height": 844})
                layout()
                page.screenshot(path=str(artifacts / "mobile.png"), full_page=True)
                page.locator('[data-command="evacuate"]').click()
                drained = wait_state('window.lifeboat.state.mode === "evacuated"')
                assert drained["activeActors"] == 0 and drained["inFlight"] == 0, drained
                assert drained["created"] == drained["delivered"], drained
                assert drained["transfers"] == drained["created"] * 4, drained
                assert all(actor["status"] == "offline" for actor in drained["actors"]), drained
                assert page.locator("#evacuated").is_visible()
                page.set_viewport_size({"width": 1500, "height": 1100})
                page.screenshot(path=str(artifacts / "evacuated.png"), full_page=True)
                page.locator('[data-command="reset"]').click()
                wait_state(f"window.lifeboat.state.run === {drained['run'] + 1} && window.lifeboat.state.delivered >= 1")
                assert not page.locator("#evacuated").is_visible()
                assert not errors, errors
                browser.close()
                (artifacts / "result.json").write_text(json.dumps({"passed": True, "evacuated": drained}, indent=2) + "\n")
                print(f"PASS: live UI congestion, recovery, supervision, evacuation, restart and responsive layout. Artifacts: {artifacts}")
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
