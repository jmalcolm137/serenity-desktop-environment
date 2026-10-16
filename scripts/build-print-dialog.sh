#!/usr/bin/env bash
# Build the PrintDialog portal dialog. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" PrintDialog
