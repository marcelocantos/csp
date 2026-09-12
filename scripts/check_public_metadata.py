#!/usr/bin/env python3
"""Check the generated gateway and public version catalogue without rewriting them."""

import contextlib
import io
from pathlib import Path
import re
import sys
import tempfile

from amalgamate import generate_gateway


def main():
    root = Path(__file__).resolve().parent.parent
    errors = []
    with tempfile.TemporaryDirectory(prefix='csp-gateway-') as scratch:
        generated = Path(scratch) / 'csp.h'
        with contextlib.redirect_stdout(io.StringIO()):
            generate_gateway(root / 'include', root / 'include/csp', generated)
        if generated.read_bytes() != (root / 'include/csp.h').read_bytes():
            errors.append('include/csp.h is stale; run make dist and stage it')

    version_re = re.compile(r'^#define (CSP_VERSION(?:_MAJOR|_MINOR|_PATCH)?) (.+)$', re.M)
    source_macros = version_re.findall((root / 'include/csp/csp.h').read_text())
    dist_macros = version_re.findall((root / 'dist/csp.h').read_text())
    source = dict(source_macros)
    dist = dict(dist_macros)
    expected_names = {'CSP_VERSION', 'CSP_VERSION_MAJOR', 'CSP_VERSION_MINOR', 'CSP_VERSION_PATCH'}
    if source.keys() != expected_names or len(source_macros) != len(expected_names):
        errors.append('include/csp/csp.h must define each of the four version macros once')
    else:
        components = '.'.join(source[f'CSP_VERSION_{part}'] for part in ('MAJOR', 'MINOR', 'PATCH'))
        if source['CSP_VERSION'] != f'"{components}"':
            errors.append('CSP_VERSION disagrees with its numeric components')
    if dist != source or len(dist_macros) != len(expected_names):
        errors.append('dist/csp.h version differs from include/csp/csp.h; run make dist')

    stability = (root / 'STABILITY.md').read_text()
    rows = re.findall(r'^\| `(CSP_VERSION(?:_MAJOR|_MINOR|_PATCH)?)` \| `([^`]+)` \|', stability, re.M)
    if dict(rows) != source or len(rows) != len(expected_names):
        errors.append('STABILITY.md version table differs from include/csp/csp.h')
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Generated gateway and public versions agree.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
