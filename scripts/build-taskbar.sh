#!/usr/bin/env bash
# Build the SerenityOS Taskbar — the SDE shell.
#
# The Taskbar is the desktop shell, so it is the SDE's: the SDE patch
# (patches/0003-sde-taskbar-build.patch) adds it to the Lagom build, and this
# script applies the SDE's patches (scripts/build.sh does that, then builds the
# named target) before building it. libgui-wayland no longer knows about the
# Taskbar. See docs/24-libgui-wayland-sde-boundary.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

exec "$SDE_ROOT/scripts/build.sh" Taskbar
