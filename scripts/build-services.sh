#!/usr/bin/env bash
# Build the session's services. See build-target.sh.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build-target.sh" \
    NotificationServer AudioServer RequestServer WebContent SQLServer
