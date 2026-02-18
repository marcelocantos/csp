#!/usr/bin/env python3
"""Remove unused #include lines identified by clang-tidy misc-include-cleaner.

Runs clang-tidy in report-only mode, parses the 'not used directly' warnings,
and deletes only those lines.  Never adds includes.
"""

import os
import re
import subprocess
import sys

UNUSED_RE = re.compile(
    r'^(.+?):(\d+):\d+: warning: included header .+ is not used directly'
)


def clean(sources, extra_args):
    for src in sources:
        abs_src = os.path.abspath(src)
        cmd = [
            'clang-tidy', '-checks=-*,misc-include-cleaner',
            src, '--',
        ] + extra_args
        result = subprocess.run(cmd, capture_output=True, text=True)
        output = result.stdout + result.stderr

        lines_to_remove = set()
        for line in output.splitlines():
            m = UNUSED_RE.match(line)
            if m and os.path.abspath(m.group(1)) == abs_src:
                lines_to_remove.add(int(m.group(2)))

        if not lines_to_remove:
            continue

        with open(src) as f:
            file_lines = f.readlines()
        with open(src, 'w') as f:
            for i, line in enumerate(file_lines, 1):
                if i not in lines_to_remove:
                    f.write(line)

        print(f'  {src}: removed {len(lines_to_remove)} unused include(s)')


if __name__ == '__main__':
    if '--' in sys.argv:
        sep = sys.argv.index('--')
        sources = sys.argv[1:sep]
        extra = sys.argv[sep + 1:]
    else:
        sources = sys.argv[1:]
        extra = []
    clean(sources, extra)
