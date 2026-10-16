#!/usr/bin/env bash
# Build the AccessDialog portal dialog. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" AccessDialog
