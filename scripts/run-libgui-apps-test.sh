#!/usr/bin/env bash
# ci-tier: full
# The applications that moved from libgui-wayland to the desktop: build the
# compositor first, then the applications, then run each against the real SDE
# compositor headless. The framework builds the shim and its own test programs;
# the desktop builds and tests the applications. (Piano is built here but run by
# run-audio-test.sh, which starts the AudioServer it needs.)
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init libgui-apps
sde_test_export_roots

SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
PDF="$SERENITY_SRC/Tests/LibPDF/complex.pdf"
MODEL="$SERENITY_SRC/Base/home/anon/Documents/3D Models/teapot.obj"

# The compositor first: the applications are tested against the desktop's real
# WindowServer, not a stand-in.
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-apps.sh" Calculator PDFViewer 3DFileViewer Piano >/dev/null

for app in Calculator PDFViewer 3DFileViewer Piano; do
    if [[ ! -x "$BUILD/bin/$app" ]]; then
        echo "==> $app was not built" >&2
        exit 1
    fi
done

sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

FAULTS='Failed to parse IPC message|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error'

# run_app NAME TITLE TIMEOUT [args ...]
run_app() {
    local name="$1" title="$2" timeout_s="$3"
    shift 3
    local log="$RUNTIME/$name.log"
    local before after
    before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
    env WAYLAND_DISPLAY="$SOCK" LIBWM_APP_BIN_DIR="$BUILD/bin" LIBWM_TRACE=1 \
        timeout "$timeout_s" "$BUILD/bin/$name" "$@" > "$log" 2>&1 || true
    after="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"

    if grep -qiE "$FAULTS" "$log"; then
        echo "  FAIL: $name faulted: $(grep -iE "$FAULTS" "$log" | head -1)"
        tail -8 "$log" | sed 's/^/      /'
        SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
        return
    fi
    sde_test_expect "$name mapped its window" test "$after" -gt "$before"
    sde_test_expect "$name presented a frame" grep -q 'presented window' "$log"
    sde_test_expect "$name window title" grep -q "toplevel mapped title='$title" "$RUNTIME/comp.log"
}

run_app Calculator 'Calculator' 5
run_app PDFViewer 'PDF Viewer' 7 "$PDF"
run_app 3DFileViewer '3D File Viewer' 8 "$MODEL"

sde_test_stop_compositor
sde_test_finish
