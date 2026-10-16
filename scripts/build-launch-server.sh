#!/usr/bin/env bash
# Build LaunchServer. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" LaunchServer
