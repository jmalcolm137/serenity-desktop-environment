#!/usr/bin/env python3
"""Move a run-*-test.sh's setup and teardown onto scripts/lib/testlib.sh.

    scripts/migrate-to-testlib.py            # report what would change
    scripts/migrate-to-testlib.py --write    # do it

A test keeps its own clients and assertions; this moves the parts every script
was repeating and getting subtly wrong on its own: the repository roots, the
private runtime directory, the cleanup (which must wait for the compositor to
exit, see docs/26-ci.md), and starting the compositor headless.

It is deliberately strict: a script is only rewritten if its roots block and its
compositor start block match the known shapes exactly. Anything else is reported
and left untouched, so a human migrates it by hand rather than a regex guessing.
"""

import glob
import os
import re
import sys

SCRIPTS = os.path.dirname(os.path.abspath(__file__))

ROOTS = re.compile(
    r'SDE_ROOT="\$\(cd "\$\(dirname "\$\{BASH_SOURCE\[0\]\}"\)/\.\." && pwd\)"\n'
    r'LIBGUI_WAYLAND_ROOT="\$\{LIBGUI_WAYLAND_ROOT:-\$SDE_ROOT/\.\./libgui-wayland\}"\n'
    r'SERENITY="\$\{SERENITY:-\$LIBGUI_WAYLAND_ROOT/serenity\}"\n'
    r'BUILD="\$\{BUILD:-\$LIBGUI_WAYLAND_ROOT/build/lagom\}"\n'
    r'(?:COMP="\$SDE_ROOT/build/WindowServer/WindowServer"\n)?'
    r'(?:export XDG_RUNTIME_DIR="\$\{XDG_RUNTIME_DIR:-/run/user/\$\(id -u\)\}"\n)?')

# The runtime directory and its trap, as one line or as the usual ordered pair.
RUNTIME = re.compile(
    r'RUNTIME="\$\(mktemp -d "[^"]*"\)"\n'
    r'(?:export SDE_STATE_DIR="\$RUNTIME"\n)?'
    r'chmod 700 "\$RUNTIME"\n'
    r'(?:export SDE_STATE_DIR="\$RUNTIME"\n)?'
    r'(?:export )?trap \'[^\']*\' EXIT\n'
    r'(?:export SDE_STATE_DIR="\$RUNTIME"\n)?')

START = re.compile(
    r'(?:SOCK="[^"]*"\n)?'
    r'env -u WAYLAND_DISPLAY -u DISPLAY \\\n'
    r'    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \\\n'
    r'(?P<env>(?:[^\n]*\\\n)*?)'
    r'    "\$COMP" -n "\$SOCK" > "\$RUNTIME/comp\.log" 2>&1 &\n'
    r'COMP_PID=\$!\n'
    r'for _ in \$\(seq 1 \d+\); do\n'
    r'    \[\[ -e "\$\{XDG_RUNTIME_DIR:-/run/user/\$\(id -u\)\}/\$SOCK" \]\] && break\n'
    r'    sleep 0\.\d+\n'
    r'done\n'
    r'(?:sleep [0-9.]+ ?&? ?\n)?')

# sde_test_compositor() already sets these.
SET_FOR_YOU = ('SERENITY_RES_ROOT=', 'LD_LIBRARY_PATH=', 'WLR_BACKENDS=',
               'WLR_HEADLESS_OUTPUTS=', 'SDE_OUTPUT_SCALE=', 'SDE_APP_BIN_DIR=')


def extra_env(block):
    kept = []
    for line in block.strip('\n').split('\n'):
        item = line.strip().rstrip('\\').strip()
        if item and not any(item.startswith(prefix) for prefix in SET_FOR_YOU):
            kept.append(item)
    return kept


def test_name(path):
    base = os.path.basename(path)
    return base[len('run-'):-len('-test.sh')]


def migrate(path):
    src = open(path).read()
    if 'scripts/lib/testlib.sh' in src:
        return None, 'already migrated'

    roots = ROOTS.search(src)
    runtime = RUNTIME.search(src)
    start = START.search(src)
    missing = [n for n, m in (('roots', roots), ('runtime+trap', runtime), ('compositor start', start)) if not m]
    if missing:
        return None, 'no exact match: ' + ', '.join(missing)
    if not (roots.end() <= runtime.start() < start.start()):
        return None, 'blocks not in the expected order'
    # A second trap would *replace* the library's cleanup (bash keeps one handler
    # per signal), so those scripts need a hand migration: they clean up more than
    # the compositor.
    if len(re.findall(r'^\s*(?:export )?trap ', src, re.M)) > 1:
        return None, 'sets more than one trap (needs a hand migration)'

    # Keep anything between the blocks (build calls, helpers) as it is.
    middle = src[roots.end():runtime.start()]
    between = src[runtime.end():start.start()]

    setup = ('SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"\n'
             '. "$SDE_ROOT/scripts/lib/testlib.sh"\n'
             f'sde_test_init {test_name(path)}\n'
             'sde_test_export_roots\n')
    env = extra_env(start.group('env'))
    call = 'sde_test_compositor' + ''.join(' ' + e for e in env) + '\n'
    return (src[:roots.start()] + setup + middle + between + call + src[start.end():],
            'ok' + (' (passes ' + ' '.join(env) + ')' if env else ''))


def main():
    write = '--write' in sys.argv
    done = 0
    for path in sorted(glob.glob(os.path.join(SCRIPTS, 'run-*-test.sh'))):
        out, status = migrate(path)
        if out and write:
            open(path, 'w').write(out)
            done += 1
        print(f'{os.path.basename(path):44s} {status}')
    if write:
        print(f'\n{done} migrated')


if __name__ == '__main__':
    main()
