#!/usr/bin/env bash
# Build the whole desktop: every piece the SDE needs, and that the tests expect to
# exist.
#
#   scripts/build-full-sde.sh              # the desktop's services, applets, tools, tests
#   scripts/build-full-sde.sh --with-apps  # plus the applications
#
# The framework (LibGUI/LibGfx/LibWM and the Lagom libraries) is *not* built here:
# build it once with libgui-wayland's scripts/build-libgui.sh, as the README's
# "Building from scratch" describes. The CI image (ci/Containerfile) bakes it so
# jobs only rebuild what their checkout changes.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITH_APPS=0
[[ "${1:-}" == "--with-apps" ]] && WITH_APPS=1

build() {
    echo "==> $1"
    # Keep the output and show it only when the step fails. Discarding it turns a
    # one-line error into a mystery: the CI log showed "==> build-apps.sh" and then
    # an exit status, with nothing to say why.
    local log status
    log="$(mktemp)"
    set +e
    "$SDE_ROOT/scripts/$1" >"$log" 2>&1
    status=$?
    set -e
    if [[ "$status" -ne 0 ]]; then
        echo "--- $1 failed (exit $status); its output follows ---" >&2
        cat "$log" >&2
        rm -f "$log"
        return "$status"
    fi
    rm -f "$log"
}

# The compositor is what every test starts.
build build-window-server.sh
# The shell, its applets and the session's services.
build build-taskbar.sh
build build-applets.sh
build build-battery-applet.sh
build build-wifi-applet.sh
build build-services.sh
# The portable tools (sde-idle, the portals) and the test clients.
build build-tools.sh
build build-tests.sh

if [[ "$WITH_APPS" -eq 1 ]]; then
    # Terminal, SystemMonitor, the demos/games/DevTools menus: the full tier's
    # tests build individual apps themselves, but doing it once is faster.
    build build-apps.sh
fi

echo "==> CI build complete"
