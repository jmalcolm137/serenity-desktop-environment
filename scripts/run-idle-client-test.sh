#!/usr/bin/env bash
# Headless idle-client test: a window with no output must not be repainted or
# configured continuously.
#
# Regression guard: the compositor once called xdg-decoration set_mode() on every
# commit. set_mode() schedules a configure, so every client was re-configured and
# repainted every frame (~60/s), pinning a core. An idle client must see almost
# no configures and commit only for the cursor blink (~2/s).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init idle-client
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

chmod 700 "$RUNTIME"
TERM_PID=""
cleanup() {
    [[ -n "$TERM_PID" ]] && kill "$TERM_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}

sde_test_compositor SDE_STATE_DIR="$RUNTIME"

# A plain Terminal, idle (an interactive shell with no output).
env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
    LD_LIBRARY_PATH="$BUILD/lib" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    SDE_APP_BIN_DIR="$BUILD/bin" "$BUILD/bin/Terminal" > "$RUNTIME/term.log" 2>&1 &
TERM_PID=$!
sleep 5

# Measure over 3 s: configures received (client log) and CPU (utime+stime ticks).
CONF_BEFORE="$(grep -c 'toplevel configure' "$RUNTIME/term.log" || true)"
COMMIT_BEFORE="$(grep -c 'presented window' "$RUNTIME/term.log" || true)"
cpu_ticks() { awk '{print $14+$15}' /proc/"$TERM_PID"/stat 2>/dev/null || echo 0; }
CPU_A="$(cpu_ticks)"
sleep 3
CONF_AFTER="$(grep -c 'toplevel configure' "$RUNTIME/term.log" || true)"
COMMIT_AFTER="$(grep -c 'presented window' "$RUNTIME/term.log" || true)"
CPU_B="$(cpu_ticks)"
HZ="$(getconf CLK_TCK)"

CONFIGURES=$((CONF_AFTER - CONF_BEFORE))
COMMITS=$((COMMIT_AFTER - COMMIT_BEFORE))
CPU_PCT="$(awk "BEGIN{printf \"%.1f\", ($CPU_B-$CPU_A)/$HZ/3*100}")"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1"; fail=1; fi; }

echo "=== idle Terminal ==="
echo "  configures in 3s: $CONFIGURES, commits in 3s: $COMMITS, CPU: ${CPU_PCT}%"
check "the idle client is not configure-stormed (configures <= 5)" "[[ '$CONFIGURES' -le 5 ]]"
check "the idle client barely repaints (commits <= 20)" "[[ '$COMMITS' -le 20 ]]"
check "the idle client uses little CPU (< 10%)" "awk 'BEGIN{exit !($CPU_PCT < 10)}'"

if [[ "$fail" -ne 0 ]]; then
    echo "--- terminal log (tail) ---"; tail -20 "$RUNTIME/term.log"
    echo "==> idle-client test FAILED"
    exit 1
fi
echo "==> idle-client test PASSED"
