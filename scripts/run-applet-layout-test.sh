#!/usr/bin/env bash
# ci-tier: fast
# Unit test for the Taskbar's applet area layout (AppletLayout.h): the [Applet]
# Order and the rectangle each applet gets, including degenerate sizes. Pure
# functions, so no Taskbar and no compositor here.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-tests.sh" libwm-applet-layout-test >/dev/null
exec "$SDE_ROOT/build/test/libwm-applet-layout-test"
