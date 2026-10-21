#!/usr/bin/env bash
# Print what a CI job is actually running on, before it does anything.
#
# The job is deliberately a hybrid, and its failures are only diagnosable if that
# is visible in the log: the *userland* is the Arch container in Meta/CI/Containerfile
# (the toolchain the tests were written against), while the *kernel* is the
# GitHub runner's — so kernel policy (perf_event_paranoid, unprivileged user
# namespaces) is Ubuntu policy, not Arch's. The framework commit matters too: the
# image bakes libgui-wayland at `main`, so a job builds the SDE's checkout against
# whatever the image was built from.
set -u

echo "=== what this job is running on ==="
printf '  container : %s\n' "$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}" || echo unknown)"
printf '  kernel    : %s %s\n' "$(uname -sr)" "$(uname -m)"
printf '  host      : %s\n' "${RUNNER_OS:-not a GitHub runner}"

# In CI this is the image's baked checkout; locally, the sibling repo.
framework="${LIBGUI_WAYLAND_ROOT:-/opt/libgui-wayland}"
serenity="${SERENITY_SRC:-$framework/serenity}"
printf '  framework : %s\n' "$(git -C "$framework" rev-parse --short HEAD 2>/dev/null || echo 'not baked into this image')"
printf '  serenity  : %s\n' "$(git -C "$serenity" rev-parse --short HEAD 2>/dev/null || echo 'not found')"

# Kernel policy the suite reacts to: the profiler test skips without
# perf_event_open, and the X11 tests fall back to no isolation without
# unprivileged user namespaces.
printf '  perf_event_paranoid : %s\n' "$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
if unshare -Urnm true 2>/dev/null; then
    printf '  unprivileged userns : yes (the X11 tests isolate themselves)\n'
else
    printf '  unprivileged userns : no (the X11 tests run unisolated — fine with no session)\n'
fi
# The tests create Wayland sockets under this; in a container it may not exist or
# not be writable, which is how the first CI run failed every compositor start.
runtime="${XDG_RUNTIME_DIR:-unset}"
if [[ "$runtime" == unset ]]; then
    printf '  runtime dir : unset (a test run supplies its own)\n'
elif [[ -w "$runtime" ]]; then
    printf '  runtime dir : %s (writable)\n' "$runtime"
else
    printf '  runtime dir : %s (NOT writable — tests must supply their own)\n' "$runtime"
fi
printf '  runner dir : %s\n' "${GITHUB_WORKSPACE:-$(pwd)}"
exit 0
