#!/usr/bin/env bash
# Build the Taskbar's applets. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" sde-applets
