#!/usr/bin/env bash
# The environment every test runs in, whoever starts it.
#
# scripts/run-all-tests.sh gives each test a controlled environment: the headless
# backend, the SerenityOS resource root, the live session's variables removed, and
# private files for what a session would otherwise share (its session directory,
# its liveness heartbeat). A test started by hand got none of that, so it could
# behave differently on its own than in the suite: tests whose programs link LibGUI
# died loading their first font ("MUST(try_load_from_uri())", no resource root),
# and a test run from inside an SDE session could reach that session's portal
# sockets, heartbeat and X server.
#
# Every scripts/run-*-test.sh sources this right after setting SDE_ROOT. Under the
# runner it does nothing (the runner has set all of it, per test); run by hand, it
# applies the same defaults. A test that needs something else sets it afterwards,
# as it would under the runner.

[[ -n "${SDE_TEST_ENV:-}" ]] && return 0
export SDE_TEST_ENV=standalone

# The backend a test's compositor uses: no DRM, no libinput (the runner's choice,
# and what CI has).
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export SDE_BACKENDS="${SDE_BACKENDS:-headless}"

# Where every LibGUI client finds its fonts and themes.
: "${LIBGUI_WAYLAND_ROOT:=$SDE_ROOT/../libgui-wayland}"
export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$LIBGUI_WAYLAND_ROOT/serenity/Base/res}"

# Nothing a test starts may reach a live session's files, sockets or services.
unset SDE_HEARTBEAT_FILE SDE_SESSION_LOG SDE_SESSION_DISPLAY SDE_WM_PORTAL_PATH \
      SDE_STATE_DIR SDE_SESSION_SHELL SDE_SESSION_IDLE_SECONDS \
      SDE_SESSION_WATCHDOG_SECONDS SDE_SESSION_SAFE_MODE SDE_LOCK_CMD \
      SDE_SESSION_IDLE_DPMS_SECONDS SDE_SESSION_COMPOSITOR_SAFE

# The test's own session directory and heartbeat. One per script, reused between
# runs, so standalone runs do not leave a new directory behind each time.
_sde_test_env_dir="${TMPDIR:-/tmp}/sde-standalone-$(id -u)/$(basename "${BASH_SOURCE[1]:-$0}" .sh)"
mkdir -p "$_sde_test_env_dir/session/portal"
chmod 700 "${TMPDIR:-/tmp}/sde-standalone-$(id -u)" 2>/dev/null || true
export SDE_SESSION_DIR="$_sde_test_env_dir/session"
export SDE_HEARTBEAT_FILE="$_sde_test_env_dir/heartbeat"
unset _sde_test_env_dir

# No X server (Xwayland's display names are global, so a test's would compete with
# a live session's), and no writes to the developer's XDG configuration.
export SDE_XWAYLAND=0
export SDE_USER_DIRS_UPDATE=0
export SDE_XDG_DATA_UPDATE=0
