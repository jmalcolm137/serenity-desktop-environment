#!/usr/bin/env bash
# Build the portal's LibGUI dialogs. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" \
    AccessDialog AppChooserDialog FileChooserDialog PrintDialog \
    ColorPicker ScreenCastChooser ScreenshotOverlay
