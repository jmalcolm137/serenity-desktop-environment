#!/usr/bin/env bash
# ci-tier: full
# Headless sanitizer run: build the compositor with ASan/UBSan and exercise it
# with the R1 smoke test.
#
# The branch this gates is the compositor's own memory safety. The Lagom framework
# is not instrumented (it is a separate build), so this finds errors in SDE code
# and in how it uses the framework's objects; a clean run is not a proof that the
# framework is clean. A separate build tree keeps the normal `build/` untouched.
#
# Runs entirely headless. The sanitizer build is the slow part, so this is in the
# full tier, not the per-push fast tier.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SANITIZE="${SDE_SANITIZE:-address}"
SAN_BUILD="${SDE_SANITIZE_BUILD_DIR:-$SDE_ROOT/build-$SANITIZE}"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-sanitizer.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

echo "==> building the sanitized compositor ($SANITIZE)"
"$SDE_ROOT/scripts/build-sanitized.sh" WindowServer > "$RUNTIME/build.log" 2>&1 || {
    echo "==> sanitizer build FAILED"
    tail -40 "$RUNTIME/build.log"
    exit 1
}

# Leak checking is off: the framework and wlroots are uninstrumented and their
# shutdown allocations are not ours to fix here. The R1 smoke test deliberately
# SIGABRTs the compositor to exercise its crash report, so do not let ASan take
# that signal over (handle_abort=0); real memory errors still report and halt.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:abort_on_error=1:handle_abort=0"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

echo "==> running the headless smoke test against the sanitized compositor"
status=0
COMP="$SAN_BUILD/WindowServer/WindowServer" "$SDE_ROOT/scripts/run-headless-test.sh" \
    > "$RUNTIME/sanitized.log" 2>&1 || status=$?

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

check "the headless smoke test passed" \
    "grep -qF '==> R1 headless smoke test PASSED' '$RUNTIME/sanitized.log'"
check "the compositor links the sanitizer runtime" \
    "ldd '$SAN_BUILD/WindowServer/WindowServer' | grep -qE 'lib(asan|ubsan)'"
check "no address-sanitizer error" \
    "! grep -qF 'ERROR: AddressSanitizer' '$RUNTIME/sanitized.log'"
check "no undefined-behaviour error" \
    "! grep -qF 'runtime error:' '$RUNTIME/sanitized.log'"

if [[ "$status" -ne 0 ]]; then
    echo "  FAIL: the smoke test exited $status"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- sanitizer run (tail) ---"
    tail -80 "$RUNTIME/sanitized.log" 2>/dev/null || true
    echo "==> sanitizer test FAILED"
    exit 1
fi
echo "==> sanitizer test PASSED"
