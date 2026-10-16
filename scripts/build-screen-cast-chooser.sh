#!/usr/bin/env bash
# Build the ScreenCastChooser portal dialog. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" ScreenCastChooser
