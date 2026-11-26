#!/usr/bin/env bash
# Build the whole desktop: every component the session and the tests need, and
# the complete SerenityOS application suite (Calculator, PDFViewer, FileManager,
# the games, demos and DevTools; the list is scripts/build-apps.sh).
#
#   scripts/build-full-sde.sh              # everything, applications included
#   scripts/build-full-sde.sh --no-apps    # skip the applications (faster iteration)
#   scripts/build-full-sde.sh --with-apps  # explicit; the default, kept for scripts
#
# This is a thin wrapper: the build itself is the top-level CMakeLists.txt. The
# framework (LibGUI/LibGfx/LibWM, the Lagom libraries) is built by the same tree's
# `lagom` step, so a fresh checkout needs nothing first.
set -euo pipefail

# The application suite is part of building the desktop. SDE_BUILD_APPS can be
# set in the environment (or on the command line) to override this.
: "${SDE_BUILD_APPS:=ON}"
case "${1:-}" in
    --with-apps) SDE_BUILD_APPS=ON; shift ;;
    --no-apps) SDE_BUILD_APPS=OFF; shift ;;
esac
export SDE_BUILD_APPS

exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" "$@"
