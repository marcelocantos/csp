#!/usr/bin/env python3
"""Verify bidirectional TLA:Module.Action correspondence tags.

Scans TLA+ specs in formal/ and C++ sources in src/ and include/ for
TLA:Module.Action tags. Reports:
  - TLA+ actions with no matching C++ tag
  - C++ tags with no matching TLA+ action
  - Summary of all matched tags

Exit code: 0 if all tags are bidirectionally matched, 1 otherwise.
"""

import glob
import os
import re
import sys

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

    ok = True

    if missing_cpp:
        ok = False
        print("TLA+ actions with no C++ tag:")
        for tag in sorted(missing_cpp):
            for path, lineno in tla_tags[tag]:
                print(f"  {tag}  ({os.path.relpath(path, root)}:{lineno})")

    if orphaned_cpp:
        ok = False
        print("C++ tags with no TLA+ action:")
        for tag in sorted(orphaned_cpp):
            for path, lineno in cpp_tags[tag]:
                print(f"  {tag}  ({os.path.relpath(path, root)}:{lineno})")

    if ok:
        print(f"All {len(matched)} TLA tags bidirectionally matched.")
    else:
        print(f"\n{len(matched)} matched, {len(missing_cpp)} missing C++, {len(orphaned_cpp)} orphaned C++.")

    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
