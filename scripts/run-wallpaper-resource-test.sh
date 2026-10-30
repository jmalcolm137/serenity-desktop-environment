#!/usr/bin/env bash
# ci-tier: fast
# Wallpaper resources (docs/04 R4.9, docs/29): the session's default wallpaper must
# actually exist and seed.
#
# On a fresh state directory the compositor seeds from SDE_WALLPAPER /
# SDE_WALLPAPER_MODE, resolving a bare name to
# <SERENITY_RES_ROOT>/wallpapers/<name>. If the file is not there it logs
# "could not load the seed wallpaper" and carries on with *no* wallpaper -- a silent
# failure that is invisible until someone notices an empty desktop.
#
# blue-grid.png was exactly that: scripts/start-sde-session.sh defaults to it, the
# pinned SerenityOS tree does not have it, and it only ever existed as one
# developer's stray, uncommitted file. The name is not interchangeable with
# grid.png either -- the two are channel-swapped variants of one pattern, blue-grid
# srgb(19,31,44) against grid's srgb(44,27,19) at the centre -- so both are wanted.
#
# The negative case is asserted as well, so the check cannot pass vacuously.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

# The default, straight out of the session script, so a rename or a typo trips this.
DEFAULT="$(sed -nE 's/.*SDE_WALLPAPER:-([^}]*)\}.*/\1/p' "$SDE_ROOT/scripts/start-sde-session.sh" | head -1)"
echo "=== session default wallpaper: '${DEFAULT:-<none found>}' ==="
check "start-sde-session.sh names a default wallpaper" "[[ -n '$DEFAULT' ]]"
check "the default exists in the resource root" "[[ -f '$RES_ROOT/wallpapers/$DEFAULT' ]]"

# Both variants are meant to be available as background images.
for w in blue-grid.png grid.png; do
    check "'$w' is available in the resource root" "[[ -f '$RES_ROOT/wallpapers/$w' ]]"
done

# Seed a fresh state directory and assert the compositor really loads it.
run_seed() { # $1 = wallpaper name, $2 = log
    local name="$1" log="$2" runtime
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-wallpaper.XXXXXX")"
    chmod 700 "$runtime"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
        SDE_STATE_DIR="$runtime" \
        SDE_WALLPAPER="$name" \
        SDE_WALLPAPER_MODE=fill \
        SERENITY_RES_ROOT="$RES_ROOT" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "sde-wallpaper-$$" > "$log" 2>&1 &
    local comp=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde-wallpaper-$$" ]] && break
        sleep 0.1
    done
    sleep 1.5
    kill -TERM -- "-$comp" 2>/dev/null || true
    wait "$comp" 2>/dev/null || true
    rm -rf "$runtime"
}

DEFAULT_LOG="$(mktemp --suffix=.log "${TMPDIR:-/tmp}/sde-wallpaper-default.XXXXXX")"
run_seed "$DEFAULT" "$DEFAULT_LOG"
echo "=== seeding the default ==="
grep -aE "seeded wallpaper|could not load the seed" "$DEFAULT_LOG" | head -3 || true
check "the compositor seeds the default wallpaper" "grep -q \"seeded wallpaper from '$RES_ROOT/wallpapers/$DEFAULT'\" '$DEFAULT_LOG'"
check "no 'could not load the seed wallpaper' warning" "! grep -q 'could not load the seed wallpaper' '$DEFAULT_LOG'"

# Negative control: a name that is not there must produce exactly that warning.
MISSING_LOG="$(mktemp --suffix=.log "${TMPDIR:-/tmp}/sde-wallpaper-missing.XXXXXX")"
run_seed "definitely-not-a-wallpaper.png" "$MISSING_LOG"
echo "=== negative control (a name that does not exist) ==="
grep -aE "could not load the seed wallpaper" "$MISSING_LOG" | head -2 || true
check "a missing wallpaper is reported (so the check has teeth)" "grep -q 'could not load the seed wallpaper' '$MISSING_LOG'"

if [[ "$fail" -ne 0 ]]; then
    echo "==> wallpaper resource test FAILED"
    exit 1
fi
echo "==> wallpaper resource test PASSED"
