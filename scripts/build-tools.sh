#!/usr/bin/env bash
# Build the SDE tools, the portal backends and the test clients. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" sde-tools
