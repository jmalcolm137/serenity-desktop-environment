#!/usr/bin/env bash
# ci-tier: full
# Help manual regression test.
#
# Help reads manuals from /usr/share/man, so checking that the manual files exist
# in a staged install is not enough: the installed Help binary must actually find
# one and render the page. Stage a full install, bind its manual tree over
# /usr/share/man in a private mount namespace, launch the installed Help, and
# assert it opens the Terminal page instead of the Error dialog.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init help-manual
sde_test_export_roots
sde_test_build WindowServer >/dev/null

ROOT="$RUNTIME/root"
PREFIX="$ROOT/usr/lib/sde"
INSTALL_LOG="$RUNTIME/install.log"
APP_PID=""
mkdir -p "$RUNTIME/session/portal"

cleanup() {
    if [[ -n "$APP_PID" ]]; then
        kill -TERM -- "-$APP_PID" 2>/dev/null || kill -TERM "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
    fi
    sde_test_cleanup
}
trap cleanup EXIT INT TERM

echo "=== stage an installed tree ==="
SDE_ROOT="$SDE_ROOT" BUILD="$BUILD" SERENITY="$SERENITY" \
    "$SDE_ROOT/scripts/install-sde.sh" --destdir "$ROOT" --prefix /usr/lib/sde \
    > "$INSTALL_LOG" 2>&1 || {
    echo "  FAIL: install-sde.sh failed"
    tail -30 "$INSTALL_LOG"
    exit 1
}
if [[ ! -f "$ROOT/usr/share/man/man1/Applications/Terminal.md" || ! -f "$ROOT/usr/share/man/man6/Hearts.md" ]]; then
    echo "  FAIL: the installed tree has no Serenity manual pages"
    exit 1
fi
echo "  ok: manual pages are in the installed tree"

# The compositor and Help use the installed prefix, not the source/build tree.
SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
SDE_SESSION_DIR="$RUNTIME/session" \
SDE_SESSION_BIN_DIR="$PREFIX/bin" \
SDE_APP_BIN_DIR="$PREFIX/bin" \
sde_test_compositor

echo "=== launch installed Help with the installed manuals mounted at /usr/share/man ==="
setsid env -u DISPLAY -u WAYLAND_DISPLAY \
    WAYLAND_DISPLAY="$SOCK" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_SESSION_BIN_DIR="$PREFIX/bin" \
    SDE_APP_BIN_DIR="$PREFIX/bin" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$PREFIX/share/res" \
    LD_LIBRARY_PATH="$PREFIX/lib" \
    unshare -Urnm sh -c 'mount --bind "$1" /usr/share/man; shift; exec "$@"' \
    sh "$ROOT/usr/share/man" "$PREFIX/bin/Help" \
    /usr/share/man/man1/Applications/Terminal.md > "$RUNTIME/help.log" 2>&1 &
APP_PID=$!

mapped=0
for _ in $(seq 1 100); do
    if grep -aFq "toplevel mapped title='Terminal(Applications) - Help'" "$RUNTIME/comp.log"; then
        mapped=1
        break
    fi
    sleep 0.1
done

if [[ "$mapped" -eq 0 ]]; then
    echo "  FAIL: Help did not open Terminal(Applications)"
    grep -aE "toplevel mapped|title='Error'|Failed to load|No such file" \
        "$RUNTIME/comp.log" "$RUNTIME/help.log" | tail -30 | sed 's/^/      /' || true
    exit 1
fi
echo "  ok: Help opened Terminal(Applications)"

if grep -aFq "toplevel mapped title='Error'" "$RUNTIME/comp.log"; then
    echo "  FAIL: Help opened an Error dialog"
    exit 1
fi
if grep -aFq "ResourceLoader: Failed load" "$RUNTIME/help.log" "$RUNTIME/comp.log"; then
    echo "  FAIL: Help could not load the manual page"
    exit 1
fi
if grep -aFq "HELP_MAP_FAIL" "$RUNTIME/help.log"; then
    echo "  FAIL: Help reported a manual failure"
    exit 1
fi

echo "==> help manual test PASSED"
