#!/usr/bin/env bash
# ci-tier: fast
# Phase F5 freedesktop.org compliance tests:
#   - LibXDG RecentlyUsed (recently-used.xbel) and SoundTheme unit tests
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-target.sh" libwm-xdg-f5-test >/dev/null
"$SDE_ROOT/build/test/libwm-xdg-f5-test"

echo "==> XDG F5 tests PASSED"
