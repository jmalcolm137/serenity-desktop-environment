#!/usr/bin/env bash
# ci-tier: fast
# Unit test for the window manager's placement rules (WindowRules.h): tile
# geometry, the drag-to-edge tile a cursor position aims for, and workspace
# visibility. Pure functions, so there is no compositor and no live session here
# — a wrong answer fails an assertion instead of a window landing one column off.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$SDE_ROOT/scripts/build-tests.sh" libwm-window-rules-test >/dev/null
exec "$SDE_ROOT/build/test/libwm-window-rules-test"
