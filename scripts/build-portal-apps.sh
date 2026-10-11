#!/usr/bin/env bash
# Build every LibGUI app the portal backend launches (R5): the file chooser,
# print dialog, permission prompt, application chooser, color picker, source
# chooser and area-selection overlay. Each app links the Serenity userland from
# the libgui-wayland build (run libgui-wayland/scripts/build-libgui.sh first).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

APPS=(
    screen-cast-chooser
    file-chooser-dialog
    print-dialog
    access-dialog
    app-chooser-dialog
    color-picker
    screenshot-overlay
)

for app in "${APPS[@]}"; do
    "$SDE_ROOT/scripts/build-$app.sh"
done
