#!/usr/bin/env bash
# Headless test for the SerenityOS demos and Magnifier (the Taskbar's Demos and
# Utilities menus), built by libgui-wayland (patch 0035). Mirrors
# run-devtools-test.sh: asserts each app's .af declares the expected category
# (which is what the Taskbar's generated menu lists, with /bin/X resolving to the
# binary next to the Taskbar), that the binary exists, and that the app launches
# into the compositor without faulting.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

# <target>:<af category>
DEMOS=(
    CatDog:Demos
    Eyes:Demos
    LibGfxDemo:Demos
    LibGfxScaleDemo:Demos
    Mandelbrot:Demos
    ModelGallery:Demos
    Screensaver:Demos
    WidgetGallery:Demos
)
# Screensavers (ExcludeFromSystemMenu, but built): Gradient, Starfield, Tubes.
SCREENSAVERS=(Gradient Starfield Tubes)
# Utilities.
EXTRA=(Magnifier:Utilities)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-demos.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$ "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$.lock 2>/dev/null || true' EXIT

fail=0
echo "=== demos/Magnifier are built and in the expected menu category ==="
for entry in "${DEMOS[@]}" "${EXTRA[@]}"; do
    app="${entry%%:*}"
    want="${entry##*:}"
    if [[ -x "$BUILD/bin/$app" ]]; then
        echo "  ok: binary $app"
    else
        echo "  MISSING: $BUILD/bin/$app"
        fail=1
    fi
    af="$SERENITY/Base/res/apps/$app.af"
    if [[ -f "$af" ]] && grep -qE "^Category=.*${want}" "$af"; then
        echo "  ok: $app.af Category=$want"
    else
        echo "  MISSING: $app.af with Category=$want"
        fail=1
    fi
done
for app in "${SCREENSAVERS[@]}"; do
    if [[ -x "$BUILD/bin/$app" ]]; then
        echo "  ok: binary $app (screensaver, hidden from the menu)"
    else
        echo "  MISSING: $BUILD/bin/$app"
        fail=1
    fi
done

SOCK="sde-demos-$$"
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

echo "=== each demo launches into the compositor ==="
for entry in "${DEMOS[@]}" "${SCREENSAVERS[@]/%/:}" "${EXTRA[@]}"; do
    app="${entry%%:*}"
    LOG="$RUNTIME/$app.log"
    before="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        timeout 5 "$BUILD/bin/$app" > "$LOG" 2>&1 || true
    after="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    if [[ "$after" -gt "$before" ]]; then
        echo "  ok: $app mapped a window"
    else
        echo "  MISSING: $app did not map a window"
        fail=1
    fi
    if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error" "$LOG"; then
        echo "  FAIL: $app reported a fault"
        fail=1
    fi
done

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"
    cat "$RUNTIME/comp.log"
    echo "==> demos test FAILED"
    exit 1
fi
echo "==> demos test PASSED"
