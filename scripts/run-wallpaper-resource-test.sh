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
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init wallpaper-resource
sde_test_export_roots
RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# The default, straight out of the session script, so a rename or a typo trips this.
DEFAULT="$(sed -nE 's/.*SDE_WALLPAPER:-([^}]*)\}.*/\1/p' "$SDE_ROOT/scripts/start-sde-session.sh" | head -1)"
echo "=== session default wallpaper: '${DEFAULT:-<none found>}' ==="
sde_test_expect "start-sde-session.sh names a default wallpaper" test -n "$DEFAULT"
sde_test_expect "the default exists in the resource root" test -f "$RES_ROOT/wallpapers/$DEFAULT"

# Both variants are meant to be available as background images.
for w in blue-grid.png grid.png; do
    sde_test_expect "'$w' is available in the resource root" test -f "$RES_ROOT/wallpapers/$w"
done

# Seed a fresh state directory and assert the compositor really loads it.
run_seed() { # $1 = case name, $2 = wallpaper name
    sde_test_case "$1"
    sde_test_case_compositor SDE_XWAYLAND=0 SDE_WALLPAPER="$2" SDE_WALLPAPER_MODE=fill SERENITY_RES_ROOT="$RES_ROOT"
    sleep 1.5
    sde_test_case_stop
}

run_seed default "$DEFAULT"
DEFAULT_LOG="$CASE_RUNTIME/comp.log"
echo "=== seeding the default ==="
grep -aE "seeded wallpaper|could not load the seed" "$DEFAULT_LOG" | head -3 || true
sde_test_check "$DEFAULT_LOG" "seeded wallpaper from '$RES_ROOT/wallpapers/$DEFAULT'" "the compositor seeds the default wallpaper"
sde_test_refute "$DEFAULT_LOG" "could not load the seed wallpaper" "no 'could not load the seed wallpaper' warning"

# Negative control: a name that is not there must produce exactly that warning.
run_seed missing "definitely-not-a-wallpaper.png"
MISSING_LOG="$CASE_RUNTIME/comp.log"
echo "=== negative control (a name that does not exist) ==="
grep -aE "could not load the seed wallpaper" "$MISSING_LOG" | head -2 || true
sde_test_check "$MISSING_LOG" "could not load the seed wallpaper" "a missing wallpaper is reported (so the check has teeth)"

sde_test_finish "$DEFAULT_LOG"
