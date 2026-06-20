#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml"]
# ///
"""Validate hygiene.yaml against repo reality (fleet hygiene initiative).

hygiene.yaml declares the repo's steady-state hygiene posture: a list of
items, each pairing an intent with a machine-checkable `evidence` pointer.
This validator resolves every pointer against what the repo actually
contains — CI jobs, Makefile targets, files, gh settings, scanners — and
fails (exit 1) when a claim has no backing reality, when a `skipped` item is
silently running, when a required `reason` is missing, or when the declared
maturity `tier` exceeds the tier the repo actually holds.

bullseye tracks aspirational state ("achieve X"); this tracks steady-state
("we maintain X"). Run via `make hygiene-check`.

The evaluator is repo-agnostic: `check_repo(root)` returns a structured
report for any repo root, so the fleet aggregator (hygiene_portfolio.py)
reuses it. Destined for a shared home (or a bullseye-sibling MCP); lives in
csp/scripts/ while the system is piloted here.

Usage: hygiene_check.py [--json] [path/to/hygiene.yaml]
       (default: ./hygiene.yaml, repo root = cwd)
"""

import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

VALID_STATE = {"enforced", "present", "manual", "planned", "skipped"}
VALID_ENFORCE = {"blocking", "warning", "informational"}
REASON_REQUIRED = {"skipped", "planned"}  # negative space must justify itself

DIM_ORDER = ["correctness", "security", "quality", "deps", "release",
             "governance", "build", "docs", "perf", "vcs", "agent"]


@dataclass
class Ctx:
    """Everything an evidence resolver needs, bound to one repo."""
    root: Path
    workflows: dict      # filename -> parsed workflow yaml
    owner_repo: str | None  # "owner/name" for gh api, or None


def load_workflows(root: Path) -> dict:
    wf = {}
    for p in sorted((root / ".github" / "workflows").glob("*.y*ml")):
        try:
            wf[p.name] = yaml.safe_load(p.read_text()) or {}
        except yaml.YAMLError as e:
            wf[p.name] = {"__error__": str(e)}
    return wf


def gh_owner_repo(root: Path) -> str | None:
    try:
        url = subprocess.run(
            ["git", "-C", str(root), "remote", "get-url", "origin"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"[/:]([^/:]+)/([^/:]+?)(?:\.git)?$", url)
    return f"{m.group(1)}/{m.group(2)}" if m else None


# --- evidence resolution ---------------------------------------------------
# Each resolver returns (ok, detail). `ok` means "this pointer resolves
# against reality". The `absent:` wrapper inverts a nested resolver.

def resolve(ctx: Ctx, ev: dict) -> tuple[bool, str]:
    if not isinstance(ev, dict) or len(ev) != 1:
        return False, f"malformed evidence (need exactly one key): {ev!r}"
    (kind, val), = ev.items()

    if kind == "absent":
        ok, detail = resolve(ctx, val)
        return (not ok), (f"present — {detail}" if ok else f"absent — {detail}")

    if kind == "ci_job":
        wf_name, _, job_spec = str(val).partition("#")
        job_id, _, matrix_name = job_spec.partition(":")
        wf = ctx.workflows.get(wf_name)
        if wf is None:
            return False, f"workflow {wf_name} not found"
        jobs = wf.get("jobs", {})
        if job_id not in jobs:
            return False, f"job '{job_id}' not in {wf_name}"
        if matrix_name:
            include = (jobs[job_id].get("strategy", {})
                       .get("matrix", {}).get("include", []) or [])
            names = [e.get("name") for e in include if isinstance(e, dict)]
            if matrix_name not in names:
                return False, f"matrix entry '{matrix_name}' not in job '{job_id}'"
            return True, f"{wf_name}#{job_id} matrix '{matrix_name}'"
        return True, f"{wf_name}#{job_id}"

    if kind == "ci_step":
        wf_name, want = val["workflow"], val["name"]
        wf = ctx.workflows.get(wf_name) or {}
        for job_id, job in (wf.get("jobs", {}) or {}).items():
            for step in (job.get("steps", []) or []):
                if isinstance(step, dict) and step.get("name") == want:
                    return True, f"{wf_name}#{job_id} step '{want}'"
        return False, f"no step named '{want}' in {wf_name}"

    if kind == "make_target":
        mk = (ctx.root / "Makefile").read_text()
        if re.search(rf"(?m)^{re.escape(str(val))}\s*:", mk):
            return True, f"Makefile target '{val}'"
        return False, f"no Makefile target '{val}'"

    if kind == "command":
        try:
            r = subprocess.run(str(val), shell=True, cwd=ctx.root,
                               capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError) as e:
            return False, f"command errored: {e}"
        return (r.returncode == 0), f"`{val}` exit {r.returncode}"

    if kind == "file":
        path = ctx.root / val["path"]
        if not path.exists():
            return False, f"missing {val['path']}"
        if "matches" in val:
            try:
                if not re.search(val["matches"], path.read_text(errors="replace")):
                    return False, f"{val['path']} present but /{val['matches']}/ not found"
            except OSError as e:
                return False, f"{val['path']}: {e}"
        return True, f"{val['path']}"

    if kind == "gh_setting":
        if ctx.owner_repo is None:
            return False, "could not derive owner/repo from git remote"
        try:
            r = subprocess.run(
                ["gh", "api", f"repos/{ctx.owner_repo}", "--jq", f".{val['key']}"],
                capture_output=True, text=True, timeout=20,
            )
        except (OSError, subprocess.SubprocessError) as e:
            return False, f"gh api failed: {e}"
        if r.returncode != 0:
            return False, f"gh api error: {r.stderr.strip()[:80]}"
        got = r.stdout.strip()
        want = json.dumps(val["equals"]) if isinstance(val["equals"], bool) else str(val["equals"])
        return (got == want), f"{val['key']}={got} (want {want})"

    if kind == "scanner":
        tool = val["tool"]
        cfg_ok = (ctx.root / val["config"]).exists() if "config" in val else True
        invoked = any(tool in yaml.safe_dump(wf) for wf in ctx.workflows.values())
        return (cfg_ok and invoked), \
            f"{tool}: config={'ok' if cfg_ok else 'missing'} invoked={'yes' if invoked else 'no'}"

    if kind == "manual":
        lv = val.get("last_verified")
        return (lv is not None), f"manual attestation (last_verified={lv})"

    return False, f"unknown evidence kind '{kind}'"


# --- per-item evaluation ---------------------------------------------------

def evaluate(ctx: Ctx, item: dict) -> dict:
    iid = item.get("id", "<no-id>")
    state = item.get("state")
    enforce = item.get("enforce")
    errors = []
    if state not in VALID_STATE:
        errors.append(f"invalid state '{state}'")
    if enforce not in VALID_ENFORCE:
        errors.append(f"invalid enforce '{enforce}'")
    if state in REASON_REQUIRED and not item.get("reason"):
        errors.append(f"state '{state}' requires a reason")

    ev = item.get("evidence")
    ok, detail = resolve(ctx, ev) if ev else (False, "no evidence declared")

    # `satisfied` = "this hygiene guarantee actually holds right now".
    #   planned  -> declared gap, never satisfied (but flag if reality
    #               outran the declaration: the thing now exists).
    #   skipped  -> satisfied iff the thing really is absent (else: silently
    #               running -> the skip is a lie).
    #   else     -> satisfied iff evidence resolves.
    advisory = None
    if state == "planned":
        satisfied = False
        # planned items assert a gap (`absent:`), so ok=True == gap still
        # open (normal). ok=False means the thing now exists -> reclassify.
        if not ok:
            advisory = "reality outran declaration — gap appears closed; reclassify"
    elif state == "skipped":
        satisfied = ok
        if not ok:
            advisory = "SKIP VIOLATED — declared skipped but present"
    else:
        satisfied = ok

    return {
        "id": iid, "dim": item.get("dim"), "tier": item.get("tier", 99),
        "state": state, "enforce": enforce, "desc": item.get("desc", ""),
        "satisfied": satisfied, "ok": ok, "detail": detail,
        "errors": errors, "advisory": advisory,
    }


# --- tier logic ------------------------------------------------------------
# A repo holds tier T iff every *blocking* item with tier <= T is satisfied.
# warning/informational gaps are reported but don't lower the held tier.

def derived_held_tier(results: list[dict], tiers: list[int]) -> tuple[int, list[dict]]:
    held = 0
    for t in sorted(tiers):
        unmet = [r for r in results
                 if r["tier"] <= t and r["enforce"] == "blocking" and not r["satisfied"]]
        if unmet:
            return held, unmet
        held = t
    return held, []


def check_repo(root: Path, doc_path: Path | None = None) -> dict:
    """Evaluate one repo's hygiene.yaml; the reusable entry point."""
    doc_path = doc_path or (root / "hygiene.yaml")
    doc = yaml.safe_load(doc_path.read_text())
    ctx = Ctx(root=root, workflows=load_workflows(root), owner_repo=gh_owner_repo(root))

    declared = doc.get("tier", 0)
    aspires = doc.get("aspires", declared)
    tier_levels = sorted(int(k) for k in (doc.get("tiers") or {1: ""}).keys())
    results = [evaluate(ctx, it) for it in doc.get("items", [])]
    held, blockers = derived_held_tier(results, tier_levels)
    config_errors = [e for r in results for e in r["errors"]]

    return {
        "repo": doc.get("repo", root.name), "declared_tier": declared,
        "aspires": aspires, "held_tier": held, "tier_ok": held >= declared,
        "blockers": blockers, "config_errors": config_errors, "results": results,
    }


# --- single-repo report (CLI) ----------------------------------------------

def main() -> int:
    argv = sys.argv[1:]
    as_json = "--json" in argv
    positional = [a for a in argv if not a.startswith("--")]
    if positional:
        doc_path = Path(positional[0]).resolve()
        root = doc_path.parent
    else:
        root = Path.cwd()
        doc_path = root / "hygiene.yaml"

    rep = check_repo(root, doc_path)
    passed = rep["tier_ok"] and not rep["config_errors"]

    if as_json:
        print(json.dumps(rep, indent=2))
        return 0 if passed else 1

    GLYPH = {True: "✓", False: "✗"}
    by_dim: dict[str, list[dict]] = {}
    for r in rep["results"]:
        by_dim.setdefault(r["dim"], []).append(r)

    print(f"hygiene: {rep['repo']}  declared tier {rep['declared_tier']}, "
          f"aspires {rep['aspires']}\n")
    for dim in sorted(by_dim, key=lambda d: DIM_ORDER.index(d) if d in DIM_ORDER else 99):
        print(f"  {dim}")
        for r in sorted(by_dim[dim], key=lambda x: x["tier"]):
            if r["state"] == "skipped" and r["satisfied"]:
                g = "⊘"  # correctly absent
            elif r["state"] == "planned":
                g = "◦"  # declared gap
            else:
                g = GLYPH[r["satisfied"]]
            print(f"    {g} [T{r['tier']} {r['enforce'][:4]}] {r['id']}: {r['detail']}")
            if r["advisory"]:
                print(f"        ⚠ {r['advisory']}")
            for e in r["errors"]:
                print(f"        ⚠ config: {e}")
        print()

    planned = [r for r in rep["results"] if r["state"] == "planned"]
    print(f"gaps to close ({len(planned)}) on the path to tier {rep['aspires']}:")
    for r in sorted(planned, key=lambda x: (x["tier"], x["id"])):
        print(f"  ◦ [T{r['tier']}] {r['id']} — {r['desc']}")
    print()

    print("─" * 60)
    print(f"held tier: {rep['held_tier']}   declared: {rep['declared_tier']}   "
          f"{'OK' if rep['tier_ok'] else 'DRIFT'}")
    if not rep["tier_ok"]:
        print(f"  declared tier {rep['declared_tier']} but only tier "
              f"{rep['held_tier']} is held; unmet blocking items below it:")
        for r in rep["blockers"]:
            print(f"    ✗ [T{r['tier']}] {r['id']}: {r['detail']}")
    if rep["config_errors"]:
        print(f"  {len(rep['config_errors'])} declaration error(s):")
        for e in rep["config_errors"]:
            print(f"    ✗ {e}")

    print(f"\n{'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
