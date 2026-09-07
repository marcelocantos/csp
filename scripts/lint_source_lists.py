#!/usr/bin/env python3
"""Lint the two hand-maintained library source lists against src/.

CSP has two build systems that each carry their own copy of the library's
translation-unit list:

    Makefile        LIB_SRCS  (the POSIX developer build)
    CMakeLists.txt  LIB_SRCS  (the Windows / consumer build)

Nothing kept them in step, and they drifted by four TUs: src/http2.cc,
src/ws.cc, src/http3.cc and src/quic.cc are compiled by the Makefile and
were absent from CMake with nothing recording that omission. This lint
makes src/ the authority and forces every divergence to be *declared*:

  1. Every src/*.cc and src/*.cpp is either in the CMake LIB_SRCS graph or
     listed in CMake's WINDOWS_OMITTED_SRCS.
  2. Every src file is either in the Makefile LIB_SRCS graph or listed in
     MAKEFILE_OMITTED below (Windows-only TUs the POSIX build cannot use).
  3. Nothing is in both a CMake LIB_SRCS list and WINDOWS_OMITTED_SRCS.
  4. A src file omitted from the CMake build has its matching
     test/<stem>.test.cc excluded by the WIN32 test filter, and nothing
     else is excluded there without an entry in WINDOWS_TEST_ONLY_OMITS.

Exit codes: 0 = clean, 1 = violations found, 2 = setup error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / 'src'
TEST_DIR = REPO_ROOT / 'test'
MAKEFILE = REPO_ROOT / 'Makefile'
CMAKELISTS = REPO_ROOT / 'CMakeLists.txt'

# Windows-only TUs: the Makefile is the POSIX build and must not compile them.
MAKEFILE_OMITTED = {'src/win_signal.cc'}

# Tests excluded from the WIN32 build for reasons other than a missing TU.
# Each entry needs a stated reason, checked by nobody but read by humans.
WINDOWS_TEST_ONLY_OMITS = {
    'test/tls.test.cc': 'PicoTLS is not built by default on Windows (CSP_TLS=OFF)',
    'test/http.test.cc': 'HTTP/1.1 serve path still broad on the Windows reactor',
}

SRC_TOKEN = re.compile(r'\b(src/[A-Za-z0-9_]+\.(?:cc|cpp))\b')


def makefile_lib_srcs(text: str) -> set[str]:
    """Collect src/ tokens from every `LIB_SRCS :=` / `+=` assignment,
    following backslash continuations. dist/ tokens fall out naturally."""
    found: set[str] = set()
    in_assignment = False
    for line in text.splitlines():
        if not in_assignment and not re.match(r'\s*LIB_SRCS\s*[:+]?=', line):
            continue
        in_assignment = line.rstrip().endswith('\\')
        found.update(SRC_TOKEN.findall(line))
    return found


def cmake_list(text: str, name: str) -> set[str]:
    """Collect src/ tokens from `set(<name> ...)` and `list(APPEND <name> ...)`
    blocks, reading on until the closing paren."""
    found: set[str] = set()
    depth = 0
    opener = re.compile(r'\b(?:set|list)\s*\(\s*(?:APPEND\s+)?' + re.escape(name) + r'\b')
    for line in text.splitlines():
        if depth == 0 and not opener.search(line):
            continue
        found.update(SRC_TOKEN.findall(line))
        depth += line.count('(') - line.count(')')
        if depth <= 0:
            depth = 0
    return found


def windows_test_filter(text: str) -> set[str]:
    """Test files excluded by the WIN32 `list(FILTER TEST_SRCS EXCLUDE ...)`
    lines, mapped back to their test/ paths."""
    excluded: set[str] = set()
    for match in re.finditer(r'list\(FILTER\s+TEST_SRCS\s+EXCLUDE\s+REGEX\s+"([^"]+)"', text):
        stem = match.group(1).replace('\\\\', '').removesuffix('.test.cc$')
        excluded.add(f'test/{stem}.test.cc')
    return excluded


def main() -> int:
    for path in (MAKEFILE, CMAKELISTS, SRC_DIR):
        if not path.exists():
            print(f'lint_source_lists: missing {path}', file=sys.stderr)
            return 2

    on_disk = {f'src/{p.name}' for p in SRC_DIR.iterdir()
               if p.suffix in ('.cc', '.cpp')}
    make_text = MAKEFILE.read_text()
    cmake_text = CMAKELISTS.read_text()

    make_srcs = makefile_lib_srcs(make_text)
    cmake_srcs = cmake_list(cmake_text, 'LIB_SRCS')
    cmake_omitted = cmake_list(cmake_text, 'WINDOWS_OMITTED_SRCS')

    violations: list[str] = []

    for src in sorted(on_disk - make_srcs - MAKEFILE_OMITTED):
        violations.append(
            f'{src}: exists in src/ but is in neither Makefile LIB_SRCS nor '
            f'MAKEFILE_OMITTED in {Path(__file__).name}')
    for src in sorted(make_srcs - on_disk):
        violations.append(f'{src}: named by Makefile LIB_SRCS but absent from src/')

    for src in sorted(on_disk - cmake_srcs - cmake_omitted):
        violations.append(
            f'{src}: exists in src/ but is in neither CMake LIB_SRCS nor '
            f'CMake WINDOWS_OMITTED_SRCS — add it to the build or declare the omission')
    for src in sorted(cmake_srcs - on_disk):
        violations.append(f'{src}: named by CMake LIB_SRCS but absent from src/')
    for src in sorted(cmake_srcs & cmake_omitted):
        violations.append(f'{src}: in both CMake LIB_SRCS and WINDOWS_OMITTED_SRCS')
    for src in sorted(cmake_omitted - on_disk):
        violations.append(f'{src}: in WINDOWS_OMITTED_SRCS but absent from src/')

    excluded_tests = windows_test_filter(cmake_text)
    for src in sorted(cmake_omitted):
        stem = Path(src).stem
        test = f'test/{stem}.test.cc'
        if (TEST_DIR / f'{stem}.test.cc').exists() and test not in excluded_tests:
            violations.append(
                f'{test}: exists and {src} is not compiled on Windows, but the '
                f'WIN32 TEST_SRCS filter does not exclude it')
    omitted_stems = {Path(s).stem for s in cmake_omitted}
    for test in sorted(excluded_tests):
        stem = Path(test).name.removesuffix('.test.cc')
        if stem not in omitted_stems and test not in WINDOWS_TEST_ONLY_OMITS:
            violations.append(
                f'{test}: excluded from the Windows test build but its TU is '
                f'compiled — add a reason to WINDOWS_TEST_ONLY_OMITS or stop excluding it')

    if violations:
        print('source list drift:')
        for v in violations:
            print(f'  {v}')
        return 1

    print(f'lint_source_lists: {len(on_disk)} src files accounted for in both builds')
    return 0


if __name__ == '__main__':
    sys.exit(main())
