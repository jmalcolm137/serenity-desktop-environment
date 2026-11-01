#!/usr/bin/env bash
# Taskbar quick-launch robustness test (patch 0009).
#
# The quick launch (the pinned buttons next to the system menu) resolves its
# entries from config, and a pinned entry can outlive the app it points at: the
# app was uninstalled, or the resource tree changed under it. An entry's name is
# its widget label and the config key it is saved under, and AppFile::name()
# VERIFYs on an empty one — so a pinned .af that is missing or invalid used to
# take the whole shell down at startup (the system menu path already filters on
# AppFile::is_valid(); the quick-launch path did not).
#
# This runs the Taskbar headless against a resource root whose apps/ directory is
# empty, so every pinned entry is missing, and asserts the shell still comes up.
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init quicklaunch
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null

# A resource root that is a faithful copy of the real one except that apps/ is
# empty: every pinned entry's .af is therefore missing.
RES="$RUNTIME/res"
mkdir -p "$RES/apps"
for entry in "${SERENITY_RES_ROOT:-$SERENITY/Base/res}"/*; do
    name="$(basename "$entry")"
    [[ "$name" == "apps" ]] && continue
    ln -s "$entry" "$RES/$name"
done

sde_test_compositor --startup "$BUILD/bin/Taskbar" \
    SERENITY_RES_ROOT="$RES" SDE_SESSION_BIN_DIR="$RUNTIME/sbin"
sde_test_wait_log "$RUNTIME/comp.log" "layer surface namespace='serenity-taskbar'" 300 || true
sleep 1

echo "=== the Taskbar with every pinned app missing ==="
sde_test_refute "$RUNTIME/comp.log" "VERIFICATION FAILED" "the Taskbar did not abort on a missing pinned entry"
sde_test_check "$RUNTIME/comp.log" "layer surface namespace='serenity-taskbar'" "the shell came up with every pinned app missing"
sde_test_finish
