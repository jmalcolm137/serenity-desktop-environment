#!/usr/bin/env bash
# WS7 font-parity confirmation (docs/10-font-parity-decision.md): assert that the
# default bitmap family (Katica) has no 14pt face, so Widget::set_font_size() is a
# no-op, while a bundled scalable face (SerenitySans) resolves at 14pt - the
# R4/G5 path to working zoom. Needs no compositor.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
TEST="$SDE_ROOT/build/test/libwm-font-parity-test"

echo "== building prerequisite =="
"$SDE_ROOT/scripts/build-tests.sh" libwm-font-parity-test >/dev/null 2>&1 || true

echo "=== font-parity test ==="
env SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$TEST"
echo "==> font-parity test PASSED"
