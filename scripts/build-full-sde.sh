#!/usr/bin/env bash
# Build the whole desktop: every component the session and the tests need.
#
#   scripts/build-full-sde.sh              # the compositor, shell, services, applets,
#                                          # portal backends, tools and tests
#   scripts/build-full-sde.sh --with-apps  # plus the application suite (in Lagom)
#
# This is a thin wrapper: the build itself is the top-level CMakeLists.txt. The
# framework (LibGUI/LibGfx/LibWM, the Lagom libraries) is built by the same tree's
# `lagom` step, so a fresh checkout needs nothing first.
set -euo pipefail

if [[ "${1:-}" == "--with-apps" ]]; then
    SDE_BUILD_APPS=ON
    export SDE_BUILD_APPS
    shift
fi

exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" "$@"
