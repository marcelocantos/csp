#!/usr/bin/env python3
"""Verify every part exported by include/csp/part/ is documented.

CLAUDE.md requires three artefacts for every combinator:

  1. a row in the catalog table of docs/reference/parts.md,
  2. a detail page under docs/reference/parts/ that the catalog row
     links to and that actually mentions the part,
  3. a row in the Combinator Reference table of dist/AGENTS-CSP.md.

Nothing enforced that, so a part could ship fully tested and entirely
undocumented: `csp::part::rand::random_bytes` did exactly that (🎯T61).

The inventory of parts is derived from the headers themselves — there is
no hand-maintained list to drift. A name counts as exported when it is
declared at namespace scope in a `csp::part...` namespace that is not
`detail` or `internal`, as either

  * a function template / function (`auto foo(...)`, `reader<T> foo(...)`), or
  * a variable template (`inline auto const foo = make_filter<...>`).

Exit codes: 0 = clean, 1 = undocumented parts found, 2 = setup error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from check_part_headers import (INFRASTRUCTURE, PART_DIR, REPO_ROOT,
                                blank_noncode, namespace_prefixes)

CATALOG = REPO_ROOT / 'docs' / 'reference' / 'parts.md'
AGENTS = REPO_ROOT / 'dist' / 'AGENTS-CSP.md'
AGENTS_TABLE_HEADING = '## Combinator Reference'

# Declarations start in column 0 in these headers; anchoring there keeps
# the match off locals, lambda bodies and class members.
FUNCTION_DECL = re.compile(
    r'(?m)^(?:inline\s+)?(?:constexpr\s+)?'
    r'[\w:]+(?:<[^;()\n]*>)?\s*[&*]{0,2}\s+'
    r'([a-z_]\w*)\s*\(')
VARIABLE_DECL = re.compile(
    r'(?m)^inline\s+(?:auto|constexpr)\s+const\s+(\w+)\s*=')


def exported_parts() -> dict[str, str]:
    """Map part name -> header path (relative to the repo root)."""
    found: dict[str, str] = {}
    for path in sorted(PART_DIR.glob('*.h')):
        if path.stem in INFRASTRUCTURE:
            continue
        code = blank_noncode(path.read_text())
        events = namespace_prefixes(code)

        for m in [*FUNCTION_DECL.finditer(code),
                  *VARIABLE_DECL.finditer(code)]:
            namespace = ''
            for end, ns in events:
                if end > m.start():
                    break
                namespace = ns
            if not namespace.startswith('csp::part'):
                continue
            if re.search(r'\b(detail|internal)\b', namespace):
                continue
            found.setdefault(m.group(1), str(path.relative_to(REPO_ROOT)))
    return found


def table_rows(text: str, heading: str | None = None) -> list[list[str]]:
    """Markdown table body rows, each split into stripped cells.

    With `heading`, only rows under that ATX heading are returned.
    """
    lines = text.splitlines()
    if heading is not None:
        try:
            start = lines.index(heading) + 1
        except ValueError:
            return []
        end = next((i for i in range(start, len(lines))
                    if lines[i].startswith('## ')), len(lines))
        lines = lines[start:end]

    rows = []
    for line in lines:
        if not line.startswith('|'):
            continue
        cells = [c.strip() for c in line.strip('|').split('|')]
        if all(re.fullmatch(r':?-+:?', c) for c in cells):
            continue  # separator
        rows.append(cells)
    return rows


def slug(heading: str) -> str:
    """GitHub-style anchor for a heading's text."""
    text = heading.lstrip('#').strip().lower()
    text = re.sub(r'[^\w\s-]', '', text)
    return re.sub(r'\s+', '-', text)


def catalog_index() -> dict[str, list[str]]:
    """Map part name -> link targets of the catalog rows naming it."""
    index: dict[str, list[str]] = {}
    for cells in table_rows(CATALOG.read_text()):
        if len(cells) < 2:
            continue
        links = re.findall(r'\[([^\]]+)\]\(([^)]+)\)', cells[0])
        names = {piece.strip().strip('`')
                 for text, _ in links for piece in text.split('/')}
        # "also `count_forever`" in the description column names the
        # siblings a single page covers.
        for cell in cells[1:]:
            names |= {c for c in re.findall(r'`([^`]+)`', cell)
                      if re.fullmatch(r'\w+', c)}
        for name in names:
            index.setdefault(name, []).extend(t for _, t in links)
    return index


def agents_names() -> set[str]:
    """Part names listed in the Combinator Reference table.

    Every column counts: a row may cover a sibling in its description
    ("`spawn_quantize` variants return endpoints") rather than give it a
    row of its own.
    """
    names = set()
    for cells in table_rows(AGENTS.read_text(), AGENTS_TABLE_HEADING):
        for cell in cells:
            for span in re.findall(r'`([^`]+)`', cell):
                # `io::lines(fd)` -> lines; `count<T>(start,stop)` -> count
                m = re.match(r'(?:\w+::)*([a-z_]\w*)', span)
                if m:
                    names.add(m.group(1))
    return names


def detail_page_covers(name: str, target: str) -> str | None:
    """None if the catalog link documents `name`, else why it doesn't."""
    file, _, anchor = target.partition('#')
    page = (CATALOG.parent / file).resolve()
    if not page.is_file():
        return f'catalog links to {target}, which does not exist'
    text = page.read_text()
    if anchor:
        headings = {slug(ln) for ln in text.splitlines()
                    if ln.startswith('#')}
        if anchor not in headings:
            return f'catalog links to {target}, but that anchor is missing'
    if not re.search(r'(?<![\w:])' + re.escape(name) + r'\b', text):
        return f'detail page {file} never mentions {name}'
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='print the exported-part inventory')
    args = ap.parse_args()

    for path in (CATALOG, AGENTS):
        if not path.is_file():
            print(f'error: {path} not found', file=sys.stderr)
            return 2

    parts = exported_parts()
    if not parts:
        print(f'error: no exported parts found under {PART_DIR}',
              file=sys.stderr)
        return 2

    catalog = catalog_index()
    agents = agents_names()

    problems: list[tuple[str, str, list[str]]] = []
    for name, header in sorted(parts.items()):
        reasons = []

        targets = catalog.get(name)
        if not targets:
            reasons.append(
                f'no row in {CATALOG.relative_to(REPO_ROOT)} names it')
        else:
            whys = [detail_page_covers(name, t) for t in targets]
            if all(whys):
                reasons.extend(dict.fromkeys(w for w in whys if w))

        if name not in agents:
            reasons.append(
                f'no row in the "{AGENTS_TABLE_HEADING.lstrip("# ")}" table '
                f'of {AGENTS.relative_to(REPO_ROOT)}')

        if reasons:
            problems.append((name, header, reasons))

    if args.verbose:
        print(f'checked {len(parts)} exported parts against '
              f'{CATALOG.relative_to(REPO_ROOT)} and '
              f'{AGENTS.relative_to(REPO_ROOT)}')
        print('parts: ' + ', '.join(sorted(parts)))

    for name, header, reasons in problems:
        print(f'{header}: {name} is undocumented', file=sys.stderr)
        for reason in reasons:
            print(f'  - {reason}', file=sys.stderr)

    if problems:
        print(f'\nFAILED: {len(problems)} part(s) undocumented.\n'
              'Every part needs a detail page under docs/reference/parts/, '
              'a catalog row in docs/reference/parts.md linking to it, and '
              'a row in the Combinator Reference table of '
              'dist/AGENTS-CSP.md (see CLAUDE.md, "Documentation").',
              file=sys.stderr)
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
