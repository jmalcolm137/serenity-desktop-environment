#!/usr/bin/env bash
# ci-tier: full
# A small fuzz harness for the compositor's INI entry point.
#
# `WindowServer --check-config` loads and validates the same WindowServer.ini the
# session uses, without a backend or a display, so it is a cheap target for a
# stream of malformed and extreme configs. Each case must exit cleanly (0 or 1):
# a hang or a signal is a parser/validation bug, not a bad file. The generator is
# deterministic (SDE_FUZZ_SEED) so a failure is reproducible.
#
# This is config fuzzing, not protocol fuzzing; see docs/08-compositor-gaps.md §7.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-60}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-config-fuzz.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# The generator deliberately mixes valid keys with garbage: our INI reader must
# fall back to defaults, never crash. Keep it dependency-light (python3 is in the
# CI image and is used by other tests).
python3 - "$RUNTIME/configs" "$SEED" "$CASES" <<'PY'
import random, sys, os, pathlib
out = pathlib.Path(sys.argv[1]); out.mkdir()
seed = int(sys.argv[2]); count = int(sys.argv[3])
rng = random.Random(seed)

sections = ["Workspaces", "Tiling", "Focus", "Effects", "Output", "Input",
            "Keyboard", "Mouse", "Applet", "Log", "Accessibility", "Compositor",
            "Taskbar", "Theme", "Fonts", "Shortcuts", "Unknown"]
keys = ["Rows", "Columns", "EdgeThreshold", "DragToEdge", "FollowsMouse",
        "WindowGeometry", "DefaultScale", "Renderer", "Layouts", "Model",
        "RepeatDelay", "RepeatRate", "StickyKeys", "HighContrast", "XWayland",
        "GroupWindows", "XDGMenu", "Level", "Path", "Name", "Order", "Acceleration",
        "ScrollStepSize", "DoubleClickSpeed", "NaturalScroll", "TapToClick",
        "LeftHanded", "ButtonsSwitched", "Nonexistent"]

def value():
    return rng.choice([
        "", "0", "1", "-1", "2", "4", "9", "16", "100", "999999999999999999999",
        "18446744073709551616", "-9223372036854775809", "1.5", "0.78", "-0.5",
        "nan", "inf", "-inf", "1e999", "true", "false", "yes", "no", "on", "off",
        "auto", "gles2", "pixman", "us", "pc105", "error", "debug", "unknown",
        "a" * 4096, "=", "[", "]", "\\", "\t", " ", "a=b", "a b c",
    ])

for i in range(count):
    lines = []
    if rng.random() < 0.2:
        lines.append("")
    for _ in range(rng.randint(1, 12)):
        roll = rng.random()
        if roll < 0.15:
            # Malformed line shapes, not just bad values.
            lines.append(rng.choice(["=", "[", "]", "[]", "[Workspaces",
                                     "Rows", "Rows=", "Rows=1=2", "; comment",
                                     "# comment", "[Unknown]"]))
            continue
        section = rng.choice(sections)
        lines.append(f"[{section}]")
        for _ in range(rng.randint(1, 8)):
            lines.append(f"{rng.choice(keys)}={value()}")
            if rng.random() < 0.1:
                # Duplicate key / conflicting value.
                lines.append(f"{rng.choice(keys)}={value()}")
    (out / f"case-{i:03d}.ini").write_text("\n".join(lines) + "\n")
PY

fail=0
hang=0
crashes=0
for config in "$RUNTIME"/configs/*.ini; do
    state="$RUNTIME/state"
    rm -rf "$state"
    mkdir -p "$state"
    cp "$config" "$state/WindowServer.ini"
    status=0
    timeout 5 env -u WAYLAND_DISPLAY -u DISPLAY \
        SDE_STATE_DIR="$state" \
        XDG_CONFIG_HOME="$state" \
        SDE_LOG_LEVEL=error \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        "$COMP" --check-config > "$RUNTIME/last.log" 2>&1 || status=$?
    if [[ "$status" -eq 124 ]]; then
        echo "  FAIL: $(basename "$config") hung"
        hang=$((hang + 1)); fail=1
    elif [[ "$status" -ge 128 ]]; then
        echo "  FAIL: $(basename "$config") died with signal $((status - 128))"
        sed 's/^/    /' "$RUNTIME/last.log" | head -10
        crashes=$((crashes + 1)); fail=1
    elif [[ "$status" -ne 0 && "$status" -ne 1 ]]; then
        echo "  FAIL: $(basename "$config") exited $status"
        sed 's/^/    /' "$RUNTIME/last.log" | head -10
        fail=1
    fi
    if [[ "$fail" -ne 0 ]]; then
        break
    fi
done

if [[ "$fail" -ne 0 ]]; then
    echo "==> config fuzz test FAILED (seed=$SEED, $crashes crashes, $hang hangs)"
    echo "--- last config ---"
    cat "$RUNTIME/state/WindowServer.ini" 2>/dev/null || true
    exit 1
fi
echo "==> config fuzz test PASSED (seed=$SEED, $CASES cases)"
