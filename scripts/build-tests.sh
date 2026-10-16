#!/usr/bin/env bash
# Build the test programs (all of them, or the ones named). See build-target.sh.
set -euo pipefail
if [[ $# -eq 0 ]]; then
    exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" sde-tests
fi
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" "$@"
