#!/usr/bin/env bash
# ci-tier: fast
# Hyperlinks are stored once per terminal; cells only carry a small index.
#
# VT::Attribute used to hold a ByteString href and an Optional<ByteString> href_id
# for every cell, which made VT::Line::Cell 56 bytes. The table-of-hyperlinks kept
# in Terminal makes the cell 28 bytes and preserves OSC 8 resolution. This test
# guards both the size (compile-time static_asserts in the payload) and the
# behavior (URL, generated ID, clearing, and distinct links).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

"$SDE_ROOT/scripts/build-tests.sh" libvt-hyperlink-test >/dev/null
TEST="$SDE_ROOT/build/test/libvt-hyperlink-test"
[[ -x "$TEST" ]] || { echo "SKIP: libvt-hyperlink-test not built"; exit 0; }

status=0
OUTPUT="$(LD_LIBRARY_PATH="$BUILD/lib" "$TEST" 2>&1)" || status=$?
if (( status != 0 )); then
    echo "  MISSING: exited $status:"
    echo "$OUTPUT" | tail -8 | sed 's/^/    /'
    echo "==> vt hyperlink test FAILED"
    exit 1
fi
echo "  ok: $(echo "$OUTPUT" | tail -1)"
echo "==> vt hyperlink test PASSED"
