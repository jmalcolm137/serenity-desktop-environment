#!/usr/bin/env bash
# ci-tier: fast
# Phase F3 freedesktop.org compliance tests:
#   - LibXDG IconTheme unit tests (index.theme, Inherits=, hicolor, pixmaps, app_id)
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-target.sh" libwm-xdg-icon-theme-test >/dev/null
"$SDE_ROOT/build/test/libwm-xdg-icon-theme-test"

echo "==> XDG F3 tests PASSED"
