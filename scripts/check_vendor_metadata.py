#!/usr/bin/env python3
"""Guard the dependency metadata duplicated by the in-tree and drop-in builds.

Keep vendor-deps.sh standalone for consumers. Version strings are CSP's existing
integration labels, not a claim that a development pin is an upstream release.
This checks agreement and hex encoding; it does not update dependencies.
"""

from pathlib import Path
import re
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent.parent
    makefile = (root / 'Makefile').read_text()
    script = (root / 'scripts/vendor-deps.sh').read_text()
    errors = []
    for dependency in ('NGHTTP2', 'NGHTTP3', 'NGTCP2'):
        values = {}
        for suffix in ('STR', 'NUM'):
            name = f'{dependency}_VERSION_{suffix}'
            make_values = re.findall(rf'^{name}\s*:=\s*(\S+)\s*$', makefile, re.M)
            script_values = re.findall(rf"^{name}='([^']+)'\s*$", script, re.M)
            if len(make_values) != 1 or make_values != script_values:
                errors.append(f'{name}: Makefile {make_values} != vendor-deps.sh {script_values}')
            else:
                values[suffix] = make_values[0]
        if len(values) == 2:
            components = values['STR'].split('.')
            if len(components) != 3 or not all(p.isdecimal() and 0 <= int(p) <= 255 for p in components):
                errors.append(f'{dependency}: invalid version string {values["STR"]}')
            elif values['NUM'] != '0x' + ''.join(f'{int(p):02x}' for p in components):
                errors.append(f'{dependency}: version number does not encode {values["STR"]}')

    # The index is the proposed pin during a dependency bump; no submodule
    # checkout or network is required (including in fresh CI checkouts).
    gitlinks = subprocess.run(['git', 'ls-files', '--stage', 'vendor/'], cwd=root,
                             text=True, capture_output=True, check=True).stdout
    for dependency, path in (
        ('PICOTLS', 'h2o/picotls'), ('NGHTTP2', 'nghttp2/nghttp2'),
        ('NGHTTP3', 'ngtcp2/nghttp3'), ('NGTCP2', 'ngtcp2/ngtcp2'),
    ):
        pins = re.findall(rf"^PIN_{dependency}='([0-9a-f]{{40}})'", script, re.M)
        recorded = re.findall(rf'^160000 ([0-9a-f]{{40}}) 0\tvendor/github\.com/{re.escape(path)}$', gitlinks, re.M)
        if len(pins) != 1 or pins != recorded:
            errors.append(f'{dependency}: fetch pin {pins} != indexed submodule {recorded}')

    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Vendored version metadata and submodule pins agree.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
