#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml"]
# ///
"""Fleet hygiene aggregator (fleet hygiene initiative).

Discovers every repo under ~/work that declares a hygiene.yaml, validates
each via hygiene_check.check_repo(), and prints a repo × dimension coverage
matrix plus a fleet-wide gap rollup. Answers "is X covered in repo Z?" and
"which repos lack X?" in one command — the question that motivated the
whole initiative.

Mirrors bullseye_portfolio's cross-repo aggregation. Sibling tool to
hygiene_check.py; both are destined for a shared home (or a bullseye-sibling
MCP) once the system leaves the csp pilot.

Usage: hygiene_portfolio.py [--json] [--root DIR]   (default root: ~/work)
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hygiene_check import DIM_ORDER, check_repo  # noqa: E402

ABBREV = {  # 4-char column headers for the matrix
    "correctness": "corr", "security": "secu", "quality": "qual",
    "deps": "deps", "release": "rels", "governance": "govn",
    "build": "buil", "docs": "docs", "perf": "perf", "vcs": "vcs",
    "agent": "agnt",
}


def discover(root: Path) -> list[Path]:
    # Workspace layout: ~/work/<host>/<org>/<repo>/hygiene.yaml
    found = {p.parent.resolve() for p in root.glob("*/*/*/hygiene.yaml")}
    found |= {p.parent.resolve() for p in root.glob("hygiene.yaml")}  # root itself
    return sorted(found, key=lambda d: d.name)


def main() -> int:
    argv = sys.argv[1:]
    as_json = "--json" in argv
    root = Path.home() / "work"
    if "--root" in argv:
        root = Path(argv[argv.index("--root") + 1]).expanduser().resolve()

    repos = discover(root)
    reports = []
    for repo_root in repos:
        try:
            reports.append(check_repo(repo_root))
        except Exception as e:  # one bad repo must not sink the fleet view
            reports.append({"repo": repo_root.name, "error": str(e),
                            "results": [], "declared_tier": 0, "held_tier": 0,
                            "aspires": 0, "tier_ok": False, "config_errors": []})

    if as_json:
        print(json.dumps({"root": str(root), "repos": reports}, indent=2))
        return 0

    if not reports:
        print(f"no hygiene.yaml found under {root}")
        return 0

    # --- coverage matrix: repo × dimension (satisfied/total) ---
    name_w = max(len(r["repo"]) for r in reports)
    name_w = max(name_w, len("repo"))
    cell_w = 5
    header = f"{'repo':<{name_w}}  " + "".join(
        f"{ABBREV[d]:>{cell_w}}" for d in DIM_ORDER) + "   tier"
    print(header)
    print("─" * len(header))

    for r in reports:
        if "error" in r:
            print(f"{r['repo']:<{name_w}}  ⚠ {r['error'][:60]}")
            continue
        by_dim: dict[str, list[dict]] = {}
        for it in r["results"]:
            by_dim.setdefault(it["dim"], []).append(it)
        cells = ""
        for d in DIM_ORDER:
            items = by_dim.get(d, [])
            if not items:
                cells += f"{'·':>{cell_w}}"
            else:
                sat = sum(1 for it in items if it["satisfied"])
                cells += f"{f'{sat}/{len(items)}':>{cell_w}}"
        drift = "" if r["tier_ok"] else " DRIFT"
        tier = f"{r['held_tier']}/{r['declared_tier']}→{r['aspires']}"
        print(f"{r['repo']:<{name_w}}  {cells}   {tier}{drift}")

    print(f"\ntier column = held/declared→aspires   "
          f"cell = satisfied/total per dimension   · = no items\n")

    # --- fleet gap rollup: which repos lack what ---
    print("fleet gaps (planned / unsatisfied items, by repo):")
    any_gap = False
    for r in reports:
        gaps = [it for it in r.get("results", [])
                if it["state"] == "planned" or
                (it["enforce"] == "blocking" and not it["satisfied"])]
        if not gaps:
            continue
        any_gap = True
        print(f"  {r['repo']}:")
        for it in sorted(gaps, key=lambda x: (x["tier"], x["id"])):
            mark = "✗" if it["enforce"] == "blocking" and not it["satisfied"] \
                and it["state"] != "planned" else "◦"
            print(f"    {mark} [T{it['tier']}] {it['id']}")
    if not any_gap:
        print("  (none — every declared item satisfied across the fleet)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
