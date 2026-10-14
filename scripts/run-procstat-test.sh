#!/usr/bin/env bash
# Regression test for the Linux ProcessStatisticsReader CPU accounting (port
# patch 0025): total_time_scheduled must advance at ~wall-clock rate, so a busy
# CPU core reads ~100% in SystemMonitor, not ~100/ncpu.
#
# Runs entirely headless (no compositor), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}}"

"$SDE_ROOT/scripts/build-tests.sh" libwm-procstat-test >/dev/null 2>&1 || true

"$SDE_ROOT/build/test/libwm-procstat-test"
