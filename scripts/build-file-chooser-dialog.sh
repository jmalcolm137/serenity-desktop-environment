#!/usr/bin/env bash
# Build the FileChooserDialog portal dialog. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" FileChooserDialog
