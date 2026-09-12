#!/usr/bin/env python3
"""Verify bidirectional TLA:Module.Action correspondence tags.

Scans TLA+ specs in formal/ and C++ sources in src/ and include/ for
TLA:Module.Action tags. Reports changed anchors and counts of matched,
missing-C++, and orphaned-C++ tag names.

The reviewed inventory in tla_tag_baseline.json freezes both directions,
including existing correspondence debt. Additions, removals and relocations
require an explicit baseline review; line-number changes do not. This checks
anchors, not semantic equivalence or model coverage. See formal/README.md.

Exit code: 0 if the inventory matches the baseline, 1 otherwise.
"""

import glob
import json
import os
import re
import sys
from pathlib import Path

TAG_RE = re.compile(r'TLA:(\w+\.\w+)')

def find_tla_tags(formal_dir):
    """Extract TLA:Module.Action tags from .tla files (excluding _Bug and _TTrace)."""
    tags = {}  # tag -> [(file, line)]
    for path in sorted(glob.glob(os.path.join(formal_dir, '*.tla'))):
        basename = os.path.basename(path)
        if '_Bug' in basename or '_TTrace' in basename:
            continue
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                for m in TAG_RE.finditer(line):
                    tag = m.group(1)
                    tags.setdefault(tag, []).append((path, lineno))
    return tags

def find_cpp_tags(*dirs):
    """Extract TLA:Module.Action tags from C++ source files."""
    tags = {}  # tag -> [(file, line)]
    patterns = ['*.cc', '*.cpp', '*.h']
    for d in dirs:
        for pat in patterns:
            for path in sorted(glob.glob(os.path.join(d, '**', pat), recursive=True)):
                with open(path) as f:
                    for lineno, line in enumerate(f, 1):
                        for m in TAG_RE.finditer(line):
                            tag = m.group(1)
                            tags.setdefault(tag, []).append((path, lineno))
    return tags

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    formal_dir = os.path.join(root, 'formal')
    src_dir = os.path.join(root, 'src')
    include_dir = os.path.join(root, 'include')

    tla_tags = find_tla_tags(formal_dir)
    cpp_tags = find_cpp_tags(src_dir, include_dir)

    all_tla = set(tla_tags.keys())
    all_cpp = set(cpp_tags.keys())

    missing_cpp = all_tla - all_cpp
    orphaned_cpp = all_cpp - all_tla
    matched = all_tla & all_cpp

    baseline_path = Path(root) / 'scripts/tla_tag_baseline.json'
    baseline = json.loads(baseline_path.read_text())
    if baseline['schema_version'] != 1:
        raise ValueError('unsupported TLA tag baseline schema')

    ok = True
    for side, tags in [('tla', tla_tags), ('cpp', cpp_tags)]:
        actual = {tag: sorted(os.path.relpath(path, root) for path, _ in sites)
                  for tag, sites in tags.items()}
        expected = baseline[side]
        for tag in sorted(actual.keys() | expected.keys()):
            if actual.get(tag) != expected.get(tag):
                ok = False
                print(f'{side}: {tag}: expected {expected.get(tag, [])}, '
                      f'found {actual.get(tag, [])}')

    print(f'{len(matched)} matched, {len(missing_cpp)} missing C++, '
          f'{len(orphaned_cpp)} orphaned C++ (correspondence debt).')
    if ok:
        print('TLA tag inventory matches the reviewed baseline.')
    else:
        print('TLA tag drift: review the affected code/spec correspondence, '
              'then commit the deliberate baseline change with the fix.')

    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
