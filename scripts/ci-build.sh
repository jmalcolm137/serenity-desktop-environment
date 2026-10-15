#!/usr/bin/env bash
# Build the SDE the way CI needs it: the pieces `scripts/run-*-test.sh` expect to
# exist, against a prebuilt framework (see ci/Containerfile and docs/26-ci.md).
#
#   scripts/ci-build.sh              # the smoke tier's pieces
#   scripts/ci-build.sh --with-apps  # plus the applications (full tier)
#
# The framework (LibGUI/LibGfx/LibWM) is *not* built here: the CI image bakes it,
# and a developer builds it once with libgui-wayland's scripts/build-libgui.sh.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITH_APPS=0
[[ "${1:-}" == "--with-apps" ]] && WITH_APPS=1

build() {
    echo "==> $1"
    "$SDE_ROOT/scripts/$1" >/dev/null
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
