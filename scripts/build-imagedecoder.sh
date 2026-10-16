#!/usr/bin/env bash
# Build ImageDecoder. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" ImageDecoder
