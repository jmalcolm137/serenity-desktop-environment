#!/usr/bin/env bash
# ci-tier: fast
# Unit test for the WindowServer.ini validation rules (ConfigRules.h):
# keybinding combos, output scales, booleans, and the input ranges. Pure
# functions, so there is no config file and no compositor here.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-tests.sh" libwm-config-rules-test >/dev/null
exec "$SDE_ROOT/build/test/libwm-config-rules-test"
