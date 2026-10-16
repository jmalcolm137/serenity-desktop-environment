#!/usr/bin/env bash
# Build the virtual-input harness the headless tests drive. Prints its path, so
# callers can use it as `VI="$(scripts/build-virtual-input.sh)"`. See build-target.sh.
set -euo pipefail
SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build}"
"$SDE_ROOT/scripts/build-target.sh" virtual-input-test >/dev/null
echo "$BUILD_DIR/input-test/virtual-input-test"
