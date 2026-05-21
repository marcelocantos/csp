#!/usr/bin/env python3
"""Lint the front-door TU and its amalgamated source files for protocol-
specific namespace references.

Rule 5 of the per-protocol DCE model (docs/design/per-protocol-dist.md §5):

    The front-door TU (dist/csp.cpp) references no protocol-specific symbols.

Concretely: no `csp::tls::`, `csp::http::`, `csp::http2::`, `csp::http3::`,
`csp::ws::`, or `csp::quic::` symbol references in dist/csp.cpp or in any
of the src/*.cc / src/*.cpp files that scripts/amalgamate.py folds into it.

If you need to glue protocol behaviour into shared code, do it through the
per-protocol enable() factory mechanism (csp_net.h) — the front-door TU
sees only an opaque protocol_option struct and a function-pointer apply(),
which leaves no name-level reference for the linker to keep the protocol
TU alive against.

Exit codes: 0 = clean, 1 = violations found, 2 = setup error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / 'src'
DIST_FILE = REPO_ROOT / 'dist' / 'csp.cpp'

# Source files that amalgamate.py folds into dist/csp.cpp. Must match the
# `excluded_sources` logic in scripts/amalgamate.py — everything in src/
# except csp_globals.cpp and the per-protocol implementations.
PROTOCOL_SRC_NAMES = {'tls.cc', 'http.cc', 'http2.cc', 'ws.cc', 'quic.cc', 'http3.cc'}
GLOBALS_SRC_NAME = 'csp_globals.cpp'

PROTOCOLS = ('tls', 'http', 'http2', 'http3', 'ws', 'quic')
PATTERN = re.compile(r'\bcsp::(' + '|'.join(PROTOCOLS) + r')::')

# Strip // line comments and /* … */ block comments (including multi-line)
# before scanning. Avoids false positives from code comments that legitimately
# reference protocol APIs ("see csp::http::serve() for details" etc.).
COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.DOTALL)


def amalgamated_sources() -> list[Path]:
    """Source files that fold into dist/csp.cpp."""
    if not SRC_DIR.is_dir():
        sys.stderr.write(f'lint_frontdoor: src/ not found at {SRC_DIR}\n')
        sys.exit(2)
    excluded = PROTOCOL_SRC_NAMES | {GLOBALS_SRC_NAME}
    return sorted(
        p for p in list(SRC_DIR.glob('*.cc')) + list(SRC_DIR.glob('*.cpp'))
        if p.name not in excluded
    )


def scan(path: Path) -> list[tuple[int, str]]:
    """Return list of (line_no, line_text) where PATTERN matches in `path`,
    after stripping comments."""
    text = path.read_text(encoding='utf-8', errors='replace')
    stripped = COMMENT_RE.sub(lambda m: '\n' * m.group(0).count('\n'), text)
    hits: list[tuple[int, str]] = []
    for n, line in enumerate(stripped.splitlines(), start=1):
        if PATTERN.search(line):
            hits.append((n, line.rstrip()))
    return hits


def main() -> int:
    targets = amalgamated_sources()
    if DIST_FILE.exists():
        targets.append(DIST_FILE)
    elif not targets:
        sys.stderr.write('lint_frontdoor: nothing to check\n')
        return 2

    violations: list[tuple[Path, int, str]] = []
    for path in targets:
        for ln, text in scan(path):
            violations.append((path, ln, text))

    if not violations:
        return 0

    for path, ln, text in violations:
        rel = path.relative_to(REPO_ROOT)
        print(f'{rel}:{ln}: {text}', file=sys.stderr)
    print('', file=sys.stderr)
    print('Rule 5 violated: the front-door TU (dist/csp.cpp) must not reference',
          file=sys.stderr)
    print('any csp::<proto>:: symbol. See docs/design/per-protocol-dist.md §5.',
          file=sys.stderr)
    print('Glue protocol behaviour through the csp::net::serve(opts) +',
          file=sys.stderr)
    print('csp::<proto>::enable() factory mechanism instead.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
