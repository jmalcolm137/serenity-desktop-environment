#!/usr/bin/env bash
# ci-tier: fast
# WifiApplet's NetworkManager client against the mock NetworkManager, without the
# applet's UI: actions, a slow, stuck, busy or restarting NetworkManager, a system
# bus that drops or comes up late, and that the applet's thread never blocks on
# any of it. The test program starts (and kills) its own private buses and mocks
# inside a scratch directory, so nothing here touches the real system bus.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
MOCK="$SDE_ROOT/build/tools/mock-networkmanager"

"$SDE_ROOT/scripts/build-tests.sh" libwm-wifi-client-test >/dev/null
mkdir -p "$SDE_ROOT/build/tools"
cc -O2 -Wall "$SDE_ROOT/Tools/mock-networkmanager.c" $(pkg-config --cflags --libs dbus-1) -o "$MOCK"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/sde-wifi-client.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

"$SDE_ROOT/build/test/libwm-wifi-client-test" "$MOCK" "$SCRATCH" "$@"
