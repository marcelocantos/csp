#!/usr/bin/env python3
"""Enforce that NOTICE attributes every vendored dependency CSP compiles.

🎯T60. The v0.30.0 licence audit found NOTICE missing an sfparse stanza and
nothing noticed: check_vendor_metadata.py only checks version pins, and
hygiene's governance.license evidence only grepped LICENSE for "Apache
License". This is the oracle that closes that loop.

Nothing here is an allowlist of dependencies. The inventory is derived:

  * Components come from the filesystem — every ``vendor/github.com/<org>/<name>``
    and ``vendor/include/<name>`` directory — cross-checked so every
    ``vendor/`` submodule recorded in .gitmodules is one of them.
  * Bundled sub-components come from nested licence files: any directory
    inside a component that carries its own LICENSE/COPYING *and* has a
    source file the build compiles is its own attribution unit (micro-ecc,
    cifra).
  * Copyright holders come from the compiled sources themselves. The list
    of compiled vendored sources is read from the build, via
    ``make print-vendor-sources`` — the Makefile is the authority on what
    is compiled, so a newly vendored source shows up here for free. This
    is what catches a flat bundled file with no directory of its own
    (nghttp2/lib/sfparse.c).

Three rules, all of which must hold:

  1. Every component and bundled sub-component has a NOTICE stanza named
     after it.
  2. Every copyright holder named in a compiled vendored source is named
     by some NOTICE copyright line (token containment, so ordering and
     year ranges don't matter).
  3. No stale stanzas: every NOTICE stanza is justified either by a live
     component or by supplying a holder that rule 2 needs.

Plus the LICENSE assertion this check inherits from hygiene's
governance.license item (so that item can point at one command) and the
verbatim dist/LICENSE + dist/NOTICE emission the drop-in set ships.

Residue this check cannot decide, verified by hand:

  * Licence *compatibility* and the correctness of the licence text quoted
    in each stanza. This checks that attribution exists, not that it is
    legally sufficient.
  * ``src/ngtcp2_crypto_picotls_minicrypto.c`` is CSP-authored but adapted
    from ngtcp2's crypto/picotls/picotls.c. It lives outside vendor/ and
    states its provenance in its own header; the ngtcp2 stanza covers it.
  * A new bundled file added *inside* an existing component whose
    copyright holders already appear in NOTICE passes silently — correctly
    so when it is more of the same upstream, but not detected if it is a
    genuinely separate work by the same holders.
  * ``scripts/build-libs.sh`` and ``CMakeLists.txt`` carry their own source
    lists over the flat vendor-deps.sh layout. The Makefile is the
    authority scanned here; ``make lint-source-lists`` guards CSP's own
    source lists, and build-libs.sh mirrors the Makefile's vendored sets
    by construction (it fetches the same pinned upstreams).
"""

from pathlib import Path
import os
import re
import subprocess
import sys

# A stanza's leading name (before any parenthetical) must contain the
# component's directory name, normalised to lowercase alphanumerics:
# "Boost.Context" covers vendor/github.com/boostorg/context.
LICENCE_FILE_RE = re.compile(r'^(LICEN[CS]E|COPYING|UNLICENSE)', re.I)
STANZA_SEPARATOR = re.compile(r'^={10,}$', re.M)
# Attribution lines. Case-sensitive on "Copyright" so the MIT boilerplate
# ("The above copyright notice ...", "copyright and related and neighboring
# rights ...") is not mistaken for an attribution. cifra states authorship
# as "Written in 2014 by ..." instead of a copyright line.
ATTRIBUTION_RE = re.compile(r'^(?:Copyright\b|Written in \d{4} by\b)')
COMMENT_PREFIX_RE = re.compile(r'^[\s*/#;]+')
EMAIL_RE = re.compile(r'<[^>]*>')
# "Kenneth MacKay. Licensed under ...", "The Chromium Authors. All rights
# reserved." — the holder is the first sentence; the rest is licence prose.
SENTENCE_END_RE = re.compile(r'\.\s+(?=[A-Z])')
STOPWORDS = frozenset(
    ('c', 'copyright', 'written', 'in', 'by', 'the', 'and', 'all', 'rights',
     'reserved'))
# Vendored trees carry upstream's own vendored trees (nghttp2/third-party,
# ngtcp2/tests). Those are attribution units only if CSP compiles them, which
# the compiled-source list decides; these prune the directory walk.
PRUNE_DIRS = frozenset(('.git', 'test', 'tests', 'doc', 'docs', 'examples'))


def normalise_name(text):
    return re.sub(r'[^a-z0-9]', '', text.lower())


def holder_tokens(line):
    """Significant name tokens of an attribution line, order-independent."""
    body = ATTRIBUTION_RE.sub('', line, count=1)
    body = body.removesuffix('*/').strip()
    body = EMAIL_RE.sub(' ', body)
    body = SENTENCE_END_RE.split(body, maxsplit=1)[0]
    tokens = (t for t in re.split(r'[^A-Za-z0-9]+', body.lower()) if t)
    return frozenset(t for t in tokens if t not in STOPWORDS and not t.isdigit())


def attribution_lines(text):
    for raw in text.split('\n'):
        line = COMMENT_PREFIX_RE.sub('', raw).strip()
        if ATTRIBUTION_RE.match(line):
            yield line


def compiled_vendored_sources(root):
    """Ask the build which third-party sources it compiles."""
    # Pinned to the full-coverage configuration, not whatever this build
    # happens to be: NOTICE ships with the source distribution and must
    # cover every dependency CSP can compile, so a local CSP_TLS=0 must not
    # make the picotls/cifra/micro-ecc attributions look unnecessary.
    # MAKEFLAGS is dropped so an outer make's command-line overrides and
    # jobserver don't reach this query.
    env = dict(os.environ)
    env.pop('MAKEFLAGS', None)
    env.pop('MFLAGS', None)
    env['CSP_TLS'] = '1'
    result = subprocess.run(['make', '-s', 'CSP_TLS=1', 'print-vendor-sources'],
                            cwd=root, text=True, capture_output=True, env=env)
    if result.returncode != 0:
        raise SystemExit(
            'make print-vendor-sources failed:\n' + result.stderr.strip())
    paths = sorted({root / line.strip()
                    for line in result.stdout.split('\n') if line.strip()})
    missing = [p for p in paths if not p.is_file()]
    if missing:
        raise SystemExit(
            'compiled vendored sources are missing from the worktree '
            '(run `git submodule update --init --recursive`):\n  '
            + '\n  '.join(str(p.relative_to(root)) for p in missing))
    return paths


def discover_components(root):
    """Top-level vendored components, from the filesystem."""
    components = {}
    for parent in sorted((root / 'vendor' / 'github.com').iterdir()):
        if parent.is_dir():
            for path in sorted(parent.iterdir()):
                if path.is_dir():
                    components[path] = path.name
    for path in sorted((root / 'vendor' / 'include').iterdir()):
        if path.is_dir():
            components[path] = path.name
    return components


def discover_bundled(root, components, sources):
    """Nested directories with their own licence file and compiled sources."""
    bundled = {}
    for component in components:
        for dirpath, dirnames, filenames in os.walk(component):
            dirnames[:] = sorted(d for d in dirnames if d not in PRUNE_DIRS)
            path = Path(dirpath)
            if path == component or not any(map(LICENCE_FILE_RE.match, filenames)):
                continue
            if any(path in source.parents for source in sources):
                bundled[path] = path.name
                dirnames.clear()
    return bundled


def parse_notice(text):
    """NOTICE stanzas as (leading name, holder token sets)."""
    stanzas = []
    # [0] is the preamble before the first separator rule, not a stanza.
    for block in STANZA_SEPARATOR.split(text)[1:]:
        lines = [line.strip() for line in block.split('\n') if line.strip()]
        if not lines:
            continue
        name = normalise_name(lines[0].split('(')[0])
        holders = [holder_tokens(line) for line in attribution_lines(block)]
        if name:
            stanzas.append((name, lines[0], holders))
    return stanzas


def main():
    root = Path(__file__).resolve().parent.parent
    errors = []

    licence = (root / 'LICENSE').read_text()
    if 'Apache License' not in licence:
        errors.append('LICENSE: expected the Apache License text')

    notice_text = (root / 'NOTICE').read_text()
    # MIT and BSD require the notices to travel with the code, so the
    # vendor drop-in set carries verbatim copies (scripts/amalgamate.py).
    for name in ('LICENSE', 'NOTICE'):
        shipped = root / 'dist' / name
        if not shipped.is_file() or shipped.read_text() != (root / name).read_text():
            errors.append(f'dist/{name} is missing or stale — run `make dist`')
    stanzas = parse_notice(notice_text)
    notice_holders = [holders for _, _, stanza in stanzas for holders in stanza]

    sources = compiled_vendored_sources(root)
    components = discover_components(root)

    # The filesystem walk must not miss a recorded submodule.
    gitmodules = (root / '.gitmodules').read_text()
    for path in re.findall(r'^\s*path\s*=\s*(vendor/\S+)\s*$', gitmodules, re.M):
        if root / path not in components:
            errors.append(f'{path}: submodule is not a discovered vendored component')

    units = dict(components)
    units.update(discover_bundled(root, components, sources))

    matched_stanzas = set()
    for path, name in sorted(units.items()):
        wanted = normalise_name(name)
        hits = [i for i, (stanza_name, _, _) in enumerate(stanzas)
                if wanted in stanza_name]
        if hits:
            matched_stanzas.update(hits)
        else:
            errors.append(
                f'{path.relative_to(root)}: vendored and compiled, but NOTICE '
                f'has no stanza named after "{name}"')

    # Every holder of a compiled vendored source must be named in NOTICE.
    used_stanzas = set()
    unattributed = {}
    for source in sources:
        for line in attribution_lines(source.read_text(errors='replace')):
            tokens = holder_tokens(line)
            if not tokens:
                continue
            hits = [i for i, (_, _, holders) in enumerate(stanzas)
                    if any(tokens <= holder for holder in holders)]
            if hits:
                used_stanzas.update(hits)
            else:
                unattributed.setdefault(line, source.relative_to(root))
    for line, source in sorted(unattributed.items()):
        errors.append(f'{source}: "{line}" is not attributed in NOTICE')

    for i, (_, title, _) in enumerate(stanzas):
        if i not in matched_stanzas and i not in used_stanzas:
            errors.append(
                f'NOTICE: stanza "{title}" matches no vendored component and '
                'no compiled source — stale?')

    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'NOTICE covers {len(units)} vendored components '
          f'({len(sources)} compiled third-party sources).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
