#!/usr/bin/env bash
# Build the native session locker (Tools/sde-lock). See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" sde-lock
