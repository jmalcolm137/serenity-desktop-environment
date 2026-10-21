#!/usr/bin/env bash
# Virtual touch/tablet test (R5): inject touch through the SDE test-only
# serenity-virtual-input protocol and assert the compositor routes it — the
# client receives touch-down/motion/up, and a drag started from a touch works
# (the touch-drag path, where the drag icon follows the finger).
#
# wlroots offers no virtual touch/tablet and the headless backend has no
# libinput, so this is how touch/pen are exercised headlessly.
#
# Setup, teardown and the pass/fail bookkeeping come from scripts/lib/testlib.sh.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init virtual-touch
sde_test_export_roots

command -v wayland-scanner >/dev/null || { echo "SKIP: no wayland-scanner"; exit 0; }

sde_test_build WindowServer sde-tools
VI="$(sde_test_vi)"

# The client this test drives is not one of the shared tools: build it here.
TOOLS_BUILD="$SDE_ROOT/build/virtual-touch-test"
mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/dnd-test.c" "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/dnd-test"

sde_test_compositor

sde_test_client env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/dnd-test" > "$RUNTIME/client.log" 2>&1
CLIENT_PID="${SDE_TEST_CLIENT_PIDS[-1]}"
sde_test_wait_log "$RUNTIME/client.log" '^mapped$' 60 || true

# Creating the virtual touch makes the seat advertise touch; wait for the client
# to bind wl_touch before injecting.
echo "=== inject: pen, then touch drag ==="
# Tablet first: the DnD client exits once its (touch) drag is cancelled, so the
# pen phase needs the window alive. The touch drag then runs last.
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --sleep 500 \
    --tablet-proximity 120 120 \
    --tablet-tip-down \
    --tablet-motion 240 180 \
    --tablet-axis 0.5 0 0 0 0 0 0 \
    --tablet-tip-up \
    --tablet-proximity-out \
    --sleep 200 \
    --touch 100 100 \
    --sleep 250 \
    --touch-motion 220 170 \
    --sleep 250 \
    --touch-up \
    --linger 400 >/dev/null 2>&1 || true
sleep 0.4

sde_test_kill "$CLIENT_PID"

# The client starts a drag from the first touch-down, so the drag grab consumes
# the following motion/up (that is the touch-drag path). Assert the down reached
# the client and the drag ran to completion.
sde_test_check "$RUNTIME/client.log" "^touch-down" "the client received touch-down"
sde_test_check "$RUNTIME/comp.log" "WindowServer: drag started" "a touch drag started"
sde_test_check "$RUNTIME/comp.log" "WindowServer: drag ended" "the touch drag ended"
sde_test_check "$RUNTIME/client.log" "^drag-(cancelled|drop|finished)$" "the drag operation resolved"
sde_test_check "$RUNTIME/comp.log" "tablet tool in proximity" "the virtual pen reached the tablet path"
sde_test_check "$RUNTIME/comp.log" "tablet tool out of proximity" "the virtual pen left proximity"
sde_test_no_fault "$RUNTIME/comp.log" "$RUNTIME/client.log"

sde_test_finish "$RUNTIME/client.log"
