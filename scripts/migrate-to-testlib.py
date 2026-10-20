#!/usr/bin/env python3
"""Move a run-*-test.sh's setup and teardown onto scripts/lib/testlib.sh.

    scripts/migrate-to-testlib.py            # report what would change
    scripts/migrate-to-testlib.py --write    # do it

A test keeps its own clients and assertions; this moves the parts every script
was repeating and getting subtly wrong on its own: the repository roots, the
private runtime directory, the cleanup (which must wait for the compositor to
exit; see docs/26-ci.md), and starting the compositor headless.

Two families are handled:

  * a one-line `trap '...' EXIT` that only stops the compositor and removes the
    runtime directory -- dropped, the library does it;
  * a `cleanup()` function and `trap cleanup EXIT` -- the function becomes
    `sde_test_cleanup_extra()`, with the lines the library now owns removed, so a
    test that also kills clients or removes an app directory keeps that code.

Anything it cannot match exactly is reported and left untouched: a second trap
would *replace* the library's cleanup, and guessing is how cleanup bugs get in.
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

RUNTIME_HEAD = re.compile(
    r'RUNTIME="\$\(mktemp -d "[^"]*"\)"\n'
    r'(?:(?:export SDE_STATE_DIR="\$RUNTIME"\n)|(?:chmod 700 "\$RUNTIME"\n)|'
    r'(?:(?:export )?trap [^\n]*\n)|(?:COMP_PID=""\n)|(?:SOCK="[^"]*"\n)|'
    r'(?:CLIENT_PID=""\n)|(?:\n)|(?:#[^\n]*\n))*?')

START = re.compile(
    r'(?:SOCK="[^"]*"\n)?'
    r'(?:setsid )?env -u WAYLAND_DISPLAY -u DISPLAY \\\n'
    r'(?P<env>(?:[^\n]*\\\n)*?)'
    r'    "\$COMP" -n "\$SOCK"(?: -s (?P<startup>"[^"]*"|\$\{?\w+\}?))? > "\$RUNTIME/comp\.log" 2>&1 &\n'
    r'COMP_PID=\$!\n'
    r'(?:\n)*'
    r'for _ in \$\(seq 1 \d+\); do\n'
    r'    \[\[ -e "\$\{XDG_RUNTIME_DIR:-/run/user/\$\(id -u\)\}/\$SOCK" \]\] && break\n'
    r'    sleep 0\.\d+\n'
    r'done\n'
    r'(?:\n)*'
    r'(?:sleep [0-9.]+ ?&? ?\n)?')

CLEANUP_FN = re.compile(r'\ncleanup\(\) \{\n(?P<body>(?:[^}\n]*\n)*?)\}\n')
STOP_FN = re.compile(r'\n(?:stop_compositor|stop_comp|kill_compositor)\(\) \{\n(?:[^}\n]*\n)*?\}\n')

# Lines the library now owns inside a cleanup body.
LIBRARY_LINE = re.compile(
    r'COMP_PID|\$\{CP\}|\bCP\b|RUNTIME|kill |pkill |wait |sleep |xdg_runtime_dir|XDG_RUNTIME_DIR')

# Only what the library always sets the same way. SDE_APP_BIN_DIR and SDE_STATE_DIR
# are kept when a script sets them: a test may point them at its own
# directories (an app dir it built symlinks into, a state dir beside the runtime).
SET_FOR_YOU = ('SERENITY_RES_ROOT=', 'LD_LIBRARY_PATH=', 'WLR_BACKENDS=',
               'WLR_HEADLESS_OUTPUTS=', 'SDE_OUTPUT_SCALE=')


def extra_env(block):
    kept = []
    for line in block.strip('\n').split('\n'):
        item = line.strip().rstrip('\\').strip()
        if item and not any(item.startswith(prefix) for prefix in SET_FOR_YOU):
            kept.append(item)
    return kept


def test_name(path):
    return os.path.basename(path)[len('run-'):-len('-test.sh')]


def migrate(path):
    src = open(path).read()
    if 'scripts/lib/testlib.sh' in src:
        return None, 'already migrated'

    roots, start = ROOTS.search(src), START.search(src)
    runtime = RUNTIME_HEAD.search(src)
    missing = [n for n, m in (('roots', roots), ('runtime', runtime), ('start', start)) if not m]
    if missing:
        return None, 'no exact match: ' + ', '.join(missing)
    if not (roots.end() <= runtime.start() < start.start()):
        return None, 'blocks not in the expected order'

    # The trap must only stop the compositor and remove the runtime directory: one
    # that also removes an app directory or kills a client has cleanup the library
    # does not own, and dropping it would leave that behind. (A cleanup() function
    # is handled above by becoming sde_test_cleanup_extra.)
    trap_match = re.search(r"^\s*(?:export )?trap '([^']*)' EXIT", src, re.M)
    if trap_match:
        body = trap_match.group(1)
        # A trap that cleans up a directory of its own (an app dir it assembled, a
        # private session dir) has cleanup the library does not own; dropping it
        # would leave that behind, so such a script is left for a hand migration.
        others = [name for name in ('APP_BIN', 'SESSION_DIR', 'PW_RUNTIME', 'BIN_DIR',
                                    'RUN_DIR', 'WORK_DIR', 'FIXTURE')
                  if name in body]
        if others:
            return None, 'trap also cleans ' + ' '.join(others)

    if re.search(r'XDG_RUNTIME_DIR="', start.group('env')):
        return None, 'moves XDG_RUNTIME_DIR (needs a hand migration)'

    if len(re.findall(r'^\s*(?:export )?trap ', src, re.M)) > 1:
        return None, 'sets more than one trap (needs a hand migration)'

    cleanup = CLEANUP_FN.search(src)
    extra_body = ''
    if cleanup:
        keep = [line for line in cleanup.group('body').splitlines()
                if line.strip() and not LIBRARY_LINE.search(line)]
        # Only a blank line and comments may be left behind; anything executable
        # is the test's own and becomes the hook.
        extra_body = '\n'.join(keep)

    out = src[:roots.start()]
    out += ('SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"\n'
            '. "$SDE_ROOT/scripts/lib/testlib.sh"\n'
            f'sde_test_init {test_name(path)}\n'
            'sde_test_export_roots\n')
    out += src[roots.end():runtime.start()]
    # SOCK is the library's; anything else kept from the runtime block was dropped
    # deliberately (trap, COMP_PID, the old mktemp).
    out += src[runtime.end():start.start()]

    env = extra_env(start.group('env'))
    startup = start.group('startup')
    call = 'sde_test_compositor'
    if startup:
        call += ' --startup ' + startup
    call += ''.join(' ' + e for e in env)
    out += call + '\n'
    out += src[start.end():]

    # Drop the old stop helpers and the trap that called them.
    # Only drop the helper if nothing else calls it: a test may stop the
    # compositor mid-run (to check what happens when it dies) rather than only in
    # its exit path.
    if not re.search(r'^\s*(?:stop_compositor|stop_comp|kill_compositor)\b', out, re.M):
        out = STOP_FN.sub('\n', out)
    out = re.sub(r'^\s*trap \'[^\']*\' EXIT\n', '', out, flags=re.M)
    out = re.sub(r'^\s*trap cleanup EXIT\n', '', out, flags=re.M)
    out = re.sub(r'^\s*COMP_PID=""\n', '', out, flags=re.M)
    if cleanup:
        moved = CLEANUP_FN.search(out)
        if not moved:
            return None, 'could not find cleanup() after rewriting'
        hook = '\n'
        if extra_body.strip() and not all(line.lstrip().startswith('#') for line in extra_body.splitlines()):
            hook = ('\n# The parts of the old cleanup() the library does not own.\n'
                    'sde_test_cleanup_extra() {\n'
                    + '\n'.join('    ' + line.strip() for line in extra_body.splitlines() if line.strip())
                    + '\n}\n')
        out = out[:moved.start()] + hook + out[moved.end():]

    return out, 'ok' + (' (startup)' if startup else '') + (' (hook)' if cleanup else '')


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
