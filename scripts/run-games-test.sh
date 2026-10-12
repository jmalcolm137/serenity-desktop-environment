#!/usr/bin/env bash
# Headless test for the SerenityOS games (the Taskbar's "Games" menu).
#
# The games are built by libgui-wayland (patch 0035). The Taskbar generates its
# "Games" category menu from the .af files under the resource root, and only
# lists an app whose executable resolves (Core::Process::resolve_executable_path,
# i.e. the binary next to the Taskbar, since the /bin/* paths are virtual here).
# So this test asserts, for each game, that the .af declares Category=Games, that
# the binary exists in the build, and that the game launches into the compositor
# without faulting.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

GAMES=(2048 BrickGame Chess ColorLines FlappyBug Flood GameOfLife Hearts MasterWord Minesweeper Snake Solitaire Spider)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-games.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$ "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$.lock 2>/dev/null || true' EXIT

fail=0
echo "=== games are built and declared in the Games category ==="
for game in "${GAMES[@]}"; do
    if [[ -x "$BUILD/bin/$game" ]]; then
        echo "  ok: binary $game"
    else
        echo "  MISSING: $BUILD/bin/$game (build it: ninja -C $BUILD ${GAMES[*]})"
        fail=1
    fi
    af="$SERENITY/Base/res/apps/$game.af"
    if [[ -f "$af" ]] && grep -qE '^Category=.*Games' "$af"; then
        echo "  ok: $game.af Category=Games"
    else
        echo "  MISSING: $game.af with Category=Games"
        fail=1
    fi
done
# Chess plays against this engine, spawned as a child process.
if [[ -x "$BUILD/bin/ChessEngine" ]]; then
    echo "  ok: binary ChessEngine"
else
    echo "  MISSING: $BUILD/bin/ChessEngine"
    fail=1
fi

SOCK="sde-games-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

echo "=== each game launches into the compositor ==="
for game in "${GAMES[@]}"; do
    LOG="$RUNTIME/$game.log"
    before="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        timeout 4 "$BUILD/bin/$game" > "$LOG" 2>&1 || true
    after="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    if [[ "$after" -gt "$before" ]]; then
        echo "  ok: $game mapped a window"
    else
        echo "  MISSING: $game did not map a window"
        fail=1
    fi
    if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error" "$LOG"; then
        echo "  FAIL: $game reported a fault"
        fail=1
    fi
done

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"
    cat "$RUNTIME/comp.log"
    echo "==> games test FAILED"
    exit 1
fi
echo "==> games test PASSED"
