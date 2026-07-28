#!/usr/bin/env python3
"""Verify every combinator header in include/csp/part/ is standalone.

Two failure modes this catches:

  1. **Not standalone-includable.** A header that only compiles because
     some other header happened to be included first.

  2. **Not standalone-instantiable.** Worse, and invisible to a plain
     include check: a template body is only resolved when instantiated,
     and ADL cannot find `csp::part::foo` from argument types outside
     namespace `csp`.  A part that delegates to another part therefore
     compiles fine as a bare include and fails the moment someone calls
     it.  `try_map` shipped in this state — its no-error overload calls
     `map<A, B>` but the header never included `csp/part/map.h`.

For each header the script generates a one-TU program that includes the
header and instantiates the part, then compiles it with `-fsyntax-only`
(which does run template instantiation) using the same compiler and
flags as the Makefile.

Instantiation snippets come from two places:

  * Variable-template parts (`template <typename T> inline auto const
    name = ...`) are detected and instantiated automatically.

  * Function-template parts need a hand-written snippet in
    INSTANTIATIONS below.  A header that *delegates* to another part —
    naming it as a template-id, or including its header — must have an
    entry: that is the class of bug described above, so the script
    fails rather than silently downgrading to an include-only check.
    Non-delegating function-template parts may omit an entry; they are
    reported as "include-only" under --verbose.

Exit codes: 0 = clean, 1 = violations found, 2 = setup error.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PART_DIR = REPO_ROOT / 'include' / 'csp' / 'part'

# part.h is the combinator infrastructure (make_filter, operator|, ...),
# not a part in its own right. It still gets an include-only check.
INFRASTRUCTURE = {'part'}

# Hand-written instantiation snippets for function-template parts, keyed
# by header basename. Each value is a statement sequence placed inside a
# function body, with `using namespace csp;` and `using namespace
# csp::part;` in scope. Required for delegating headers (see module
# docstring); optional otherwise.
#
# Keep snippets minimal — they only need to force instantiation, not to
# exercise behaviour. That is what test/ is for.
INSTANTIATIONS: dict[str, str] = {
    'batch': 'auto f = batch<int>(4);',

    # Requires a range whose iterators support ->read() (see the
    # `decltype` constraint on chain's third template parameter).
    'chain': (
        'std::vector<reader<int>> rs;\n'
        'auto f = chain<int>(std::move(rs));'
    ),

    'chunk_by': 'auto f = chunk_by<int>([](int a, int b) { return a == b; });',

    'collect': (
        'int buf[1] = {};\n'
        'auto f = collect<int>(buf);'
    ),

    # The no-combiner overload deduces Ts from the reader<Ts>... pack.
    # The combining-function overload's own comment says it needs
    # explicit type parameters — trailing F after the pack defeats
    # deduction.
    'combine_latest': (
        'auto a = combine_latest(reader<int>{}, reader<double>{});\n'
        'auto b = combine_latest<int, double>(\n'
        '    reader<int>{}, reader<double>{},\n'
        '    [](int& x, double& y) { return int(x + y); });'
    ),

    'conflate': 'auto f = conflate<int>([](int a, int b) { return a + b; });',

    'count': (
        'auto f = count<int>(0, 10);\n'
        'auto g = count_forever<int>(0);'
    ),

    'debounce': 'auto f = debounce<int>(std::chrono::milliseconds(10));',

    'default_if_empty': 'auto f = default_if_empty<int>(0);',

    'delay': 'auto f = delay<int>(std::chrono::milliseconds(10));',

    # Includes csp/part/unzip.h.
    'demux': (
        'reader<std::variant<int, double>> in;\n'
        'auto f = demux(std::move(in));'
    ),

    'distinct': 'auto f = distinct<int>();',

    'enumerate': (
        'std::vector<int> v{1, 2, 3};\n'
        'auto f = enumerate(v);\n'
        'auto g = enumerate<int>({1, 2, 3});\n'
        'auto h = cycle(v);\n'
        'auto k = cycle<int>({1, 2, 3});'
    ),

    'fallback': (
        'std::vector<reader<int>> v;\n'
        'auto f = fallback<int>(std::move(v));'
    ),

    'first_last': (
        'auto f = first<int>(2);\n'
        'auto g = last<int>(2);\n'
        'auto h = skip_first<int>(2);\n'
        'auto k = skip_last<int>(2);'
    ),

    'first_wins': (
        'std::vector<reader<int>> v;\n'
        '[[maybe_unused]] auto t = first_wins<int>(std::move(v));'
    ),

    'flat_map': (
        'auto f = flat_map<int, int>(\n'
        '    [](int) { return reader<int>{}; });'
    ),

    'foreach_emit': (
        'auto f = foreach_emit<int, int, int>(\n'
        '    0,\n'
        '    [](int s, int t) { return s + t; },\n'
        '    [](int s) { return s; });'
    ),

    'frame': 'auto f = frame<int>(4, std::chrono::milliseconds(10));',

    'gate': 'auto r = gate<int>(reader<int>{}, reader<bool>{});',

    'group_by': (
        'auto r = group_by<int>(\n'
        '    reader<int>{}, [](int const& x) { return x % 2; });'
    ),

    'interleave': (
        'std::vector<reader<int>> v;\n'
        'auto f = interleave<int>(std::move(v));'
    ),

    'join': (
        'std::vector<reader<int>> v;\n'
        'join<int>(std::move(v));'
    ),

    'killswitch': 'auto f = killswitch<int>(reader<>{});',

    'map': 'auto f = map<int>([](int n) { return n + 1; });',

    'merge': (
        'std::vector<reader<int>> v;\n'
        'auto f = merge<int>(std::move(v));'
    ),

    'metrics': 'auto p = metrics<int>(reader<int>{});',

    'mux': 'auto m = mux(reader<int>{}, reader<double>{});',

    'nwise': 'auto f = nwise<3, int>();',

    'pace': 'auto f = pace<int>(reader<>{});',

    'parallel_map': 'auto f = parallel_map<int>(4, [](int x) { return x; });',

    'partition': (
        'auto v1 = partition<int>(\n'
        '    reader<int>{}, 3, [](int x) { return size_t(x); });\n'
        'auto v2 = partition<int>(\n'
        '    reader<int>{}, [](int x) { return x > 0; });'
    ),

    'quantify': (
        'auto f = any_of<int>([](int x) { return x > 0; });\n'
        'auto g = all_of<int>([](int x) { return x > 0; });'
    ),

    'quantize': (
        'auto f1 = quantize<int>(reader<int>{}, reader<int>{}, writer<int>{});\n'
        'auto w1 = spawn_quantize<int>(reader<int>{}, writer<int>{});\n'
        'auto f2 = quantize<int>(reader<int>{}, 5, writer<int>{});\n'
        'auto r2 = spawn_quantize<int>(reader<int>{}, 5);\n'
        'auto w3 = spawn_quantize<int>(5, writer<int>{});'
    ),

    'race': (
        'std::vector<reader<int>> v;\n'
        'auto f = race<int>(std::move(v));'
    ),

    'random': (
        'using namespace csp::part::rand;\n'
        'auto a = uniform_int<int>(0, 10);\n'
        'auto b = uniform_real<double>(0.0, 1.0);\n'
        'auto c = bernoulli();\n'
        'auto d = normal<double>();\n'
        'std::vector<int> choices{1, 2, 3};\n'
        'auto e = choice<int>(choices);\n'
        'auto g = random_bytes(16);\n'
        'auto h = shuffle<int>(4);'
    ),

    'reduce': 'auto f = reduce<int>(0, [](int acc, int x) { return acc + x; });',

    'reorder': (
        'auto f = reorder<int>(\n'
        '    std::function<size_t(int const&)>(\n'
        '        [](int const& x) { return size_t(x); }));'
    ),

    'round_robin': 'auto v = round_robin<int>(reader<int>{}, 3);',

    'rpc': (
        'auto c1 = rpc_client(writer<std::tuple<int>>{}, reader<int>{});\n'
        'auto s1 = rpc_server(reader<std::tuple<int>>{}, writer<int>{},\n'
        '    [](int x) { return x; });\n'
        'auto c2 = rpc_client(\n'
        '    writer<std::pair<std::tuple<int>, writer<int>>>{});\n'
        'auto s2 = rpc_server(\n'
        '    reader<std::pair<std::tuple<int>, writer<int>>>{},\n'
        '    [](int x) { return x; });'
    ),

    'sample': 'auto f = sample<int>(reader<int>{}, reader<>{});',

    'scan': 'auto f = scan<int>(0, [](int acc, int x) { return acc + x; });',

    'share': 'auto r = share<int>(reader<int>{});',

    'sink': (
        'auto f = sink<int>([](int x) { (void)x; });\n'
        'int v = 0;\n'
        'auto g = sinkhole<int>(v);'
    ),

    'skip_while': 'auto f = skip_while<int>([](int x) { return x < 0; });',

    'slide': (
        'auto w1 = slide<int>(\n'
        '    reader<int>{}, [](int a, int b) { return a < b; });\n'
        'auto w2 = slide<int>(reader<int>{}, size_t(4));'
    ),

    'sort_merge': (
        'std::vector<reader<int>> v;\n'
        'auto f = sort_merge<int>(std::move(v));'
    ),

    'stride': 'auto f = stride<int>(2);',

    'take_until': 'auto f = take_until<int>([](int x) { return x > 5; });',

    'take_while': 'auto f = take_while<int>([](int x) { return x < 5; });',

    'tee': 'auto f = tee<int>(writer<int>{});',

    'throttle': 'auto f = throttle<int>(reader<>{});',

    'timeout': 'auto f = timeout<int>(std::chrono::milliseconds(10));',

    'timer': (
        'auto a = timer(reader<duration>{});\n'
        'auto b = timer(reader<time_point>{});'
    ),

    'transpose': (
        'std::vector<reader<int>> v;\n'
        'auto f = transpose<int>(std::move(v));'
    ),

    # Delegates to map<A, B> via its no-error overload — the motivating
    # case for this whole script.
    'try_map': (
        'writer<std::exception_ptr> err;\n'
        'auto a = try_map<int>([](int n) { return n + 1; }, std::move(err));\n'
        'auto b = try_map<int>([](int n) { return n + 1; });'
    ),

    'unique': 'auto f = unique<int>();',

    'unzip': (
        'auto u1 = unzip(reader<std::tuple<int, double>>{});\n'
        'auto u2 = unzip(\n'
        '    reader<int>{}, [](int x) { return std::tuple{x, x}; });'
    ),

    'where': 'auto f = where<int>([](int x) { return x > 0; });',

    'window': 'auto f = window<int>(4);',

    # zip's combining-function overload requires explicit type
    # parameters (trailing F after the pack defeats deduction).
    'zip': (
        'auto a = zip<int, double>(\n'
        '    reader<int>{}, reader<double>{},\n'
        '    [](int x, double y) { return x + y; });\n'
        'auto b = zip(reader<int>{}, reader<double>{});'
    ),
}


def blank_noncode(text: str) -> str:
    """Replace comments and string literals with spaces, preserving offsets.

    Length preservation matters: offsets from a regex over the result
    index the original source.
    """
    def blank(m: re.Match) -> str:
        return re.sub(r'[^\n]', ' ', m.group(0))

    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                  blank, text, flags=re.S)


def namespace_prefixes(code: str) -> list[tuple[int, str]]:
    """Namespace in effect after each brace event, as (offset, prefix)."""
    events: list[tuple[int, str]] = []
    stack: list[str | None] = []
    for m in re.finditer(r'\bnamespace\s+([\w:]+)\s*\{|[{}]', code):
        if m.group(1) is not None:
            stack.append(m.group(1))
        elif m.group(0) == '{':
            stack.append(None)
        elif stack:
            stack.pop()
        events.append((m.end(), '::'.join(n for n in stack if n)))
    return events


def variable_parts(source: str) -> list[tuple[str, bool]]:
    """Return (qualified_name, is_template) for `inline auto const NAME =`.

    `is_template` is True when the declaration is preceded by a template
    parameter list, in which case the part is used as `NAME<T>`.
    """
    code = blank_noncode(source)
    events = namespace_prefixes(code)

    found = []
    for m in re.finditer(
            r'(?:(template\s*<[^\n]*>)\s*\n\s*)?'
            r'inline\s+(?:auto|constexpr)\s+const\s+(\w+)\s*=',
            code):
        prefix = ''
        for end, ns in events:
            if end > m.start():
                break
            prefix = ns
        name = f'{prefix}::{m.group(2)}' if prefix else m.group(2)
        found.append((name, m.group(1) is not None))
    return found


def delegated_parts(stem: str, source: str, all_stems: set[str]) -> set[str]:
    """Parts that this header's implementation depends on.

    Two signals, both meaning "this header's templates reach into
    another part at instantiation time":

      * the other part's name in template-id position (`foo<`), taking
        care not to match `foo <<` or `foo <=`;
      * an `#include <csp/part/foo.h>` of another part.
    """
    hits = {m for m in re.findall(r'#include\s*<csp/part/(\w+)\.h>', source)}
    hits -= INFRASTRUCTURE

    body = blank_noncode(source)
    for other in all_stems:
        if re.search(r'(?<![\w:])' + re.escape(other) + r'\s*<(?![<=])', body):
            hits.add(other)

    hits.discard(stem)
    return hits


def compiler_command() -> list[str]:
    """The Makefile's compiler invocation, honouring CXX and CSP_TLS."""
    cmd = shlex.split(os.environ.get('CXX') or 'c++')
    if not any(a.startswith('-std=') for a in cmd):
        cmd.append('-std=c++20')
    if not any(a.startswith('-stdlib=') for a in cmd):
        cmd.append('-stdlib=libc++')

    cmd += ['-Wall', '-Wextra', '-Wno-unused-parameter']
    if os.environ.get('CSP_TLS', '1') != '0':
        cmd.append('-DCSP_TLS')
    cmd += ['-I', str(REPO_ROOT / 'include'),
            '-I', str(REPO_ROOT / 'vendor' / 'include')]
    return cmd


def build_tu(stem: str, snippet: str) -> str:
    return (
        f'#include <csp/part/{stem}.h>\n'
        '\n'
        '#include <exception>\n'
        '#include <string>\n'
        '#include <utility>\n'
        '#include <variant>\n'
        '#include <vector>\n'
        '\n'
        'namespace {\n'
        '[[maybe_unused]] void csp_part_header_check() {\n'
        '    using namespace csp;\n'
        '    using namespace csp::part;\n'
        f'{snippet}\n'
        '}\n'
        '}  // namespace\n'
    )


def check_header(path: Path, snippet: str, cmd: list[str],
                 workdir: Path) -> tuple[str, str | None]:
    """Compile one generated TU. Returns (stem, error text or None)."""
    stem = path.stem
    tu = workdir / f'{stem}.check.cc'
    tu.write_text(build_tu(stem, snippet))

    proc = subprocess.run(
        cmd + ['-fsyntax-only', str(tu)],
        capture_output=True, text=True, cwd=REPO_ROOT)
    if proc.returncode != 0:
        return stem, proc.stderr.strip()
    return stem, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='list every header and how it was checked')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4,
                    help='parallel compilations (default: CPU count)')
    args = ap.parse_args()

    if not PART_DIR.is_dir():
        print(f'error: {PART_DIR} not found', file=sys.stderr)
        return 2

    headers = sorted(PART_DIR.glob('*.h'))
    if not headers:
        print(f'error: no headers in {PART_DIR}', file=sys.stderr)
        return 2

    all_stems = {p.stem for p in headers}

    plan: list[tuple[Path, str]] = []
    include_only: list[str] = []
    missing: list[tuple[str, set[str]]] = []

    for path in headers:
        stem = path.stem
        source = path.read_text()

        if stem in INSTANTIATIONS:
            plan.append((path, '    ' + INSTANTIATIONS[stem].replace(
                '\n', '\n    ')))
            continue

        parts = variable_parts(source)
        if parts:
            lines = []
            for i, (name, is_template) in enumerate(parts):
                use = f'::{name}<int>' if is_template else f'::{name}'
                lines.append(
                    f'    [[maybe_unused]] auto const& v{i} = {use};')
            plan.append((path, '\n'.join(lines)))
            continue

        # No instantiation available. That is only acceptable when the
        # header's templates don't reach into another part.
        delegates = delegated_parts(stem, source, all_stems)
        if delegates and stem not in INFRASTRUCTURE:
            missing.append((stem, delegates))
        include_only.append(stem)
        plan.append((path, '    // include-only'))

    cmd = compiler_command()
    failures: list[tuple[str, str]] = []

    with tempfile.TemporaryDirectory(prefix='csp-part-check-') as tmp:
        workdir = Path(tmp)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = [pool.submit(check_header, path, snippet, cmd, workdir)
                       for path, snippet in plan]
            for fut in concurrent.futures.as_completed(futures):
                stem, err = fut.result()
                if err:
                    failures.append((stem, err))

    if args.verbose:
        instantiated = len(plan) - len(include_only)
        print(f'checked {len(plan)} part headers '
              f'({instantiated} instantiated, '
              f'{len(include_only)} include-only)')
        if include_only:
            print('include-only: ' + ', '.join(sorted(include_only)))

    for stem, delegates in sorted(missing):
        print(f'{PART_DIR.relative_to(REPO_ROOT)}/{stem}.h: '
              f'delegates to {", ".join(sorted(delegates))} but has no '
              f'instantiation snippet.\n'
              f'  Add an INSTANTIATIONS["{stem}"] entry to '
              f'{Path(__file__).relative_to(REPO_ROOT)} — an include-only '
              f'check cannot catch a missing include behind a template.',
              file=sys.stderr)

    for stem, err in sorted(failures):
        print(f'{PART_DIR.relative_to(REPO_ROOT)}/{stem}.h: '
              f'standalone check failed\n{err}\n', file=sys.stderr)

    if missing or failures:
        print(f'FAILED: {len(failures)} header(s) failed to compile, '
              f'{len(missing)} missing instantiation coverage',
              file=sys.stderr)
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
