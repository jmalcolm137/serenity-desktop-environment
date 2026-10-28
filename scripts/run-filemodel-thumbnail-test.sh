#!/usr/bin/env bash
# ci-tier: fast
# FileSystemModel must decode thumbnails without blocking the shared background
# thread, and a second request for a file that is already decoding must join the
# in-flight job. The payload (Tests/FileSystemModelThumbnailTest.cpp) writes a
# few PNGs, requests each icon twice, pumps the event loop, and checks that every
# node gets a 32x32 thumbnail with one job per file.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init filemodel-thumbnail
sde_test_export_roots

TEST="$SDE_ROOT/build/test/filemodel-thumbnail-test"
IMAGE_DECODER="$SDE_ROOT/build/ImageDecoder/ImageDecoder"
IMAGE_PID=""

"$SDE_ROOT/scripts/build-imagedecoder.sh" >/dev/null 2>&1 || true
"$SDE_ROOT/scripts/build-tests.sh" filemodel-thumbnail-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: filemodel-thumbnail-test not built"; exit 0; }
[[ -x "$IMAGE_DECODER" ]] || { echo "SKIP: ImageDecoder not built"; exit 0; }

# LibGUI::Application needs a session (clipboard endpoint) and the shim needs a
# compositor. A real ImageDecoder service is started too: the test asserts that
# the decode request reaches it while the process's shared background thread is
# deliberately blocked.
SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

sde_test_cleanup_extra() {
    [[ -n "$IMAGE_PID" ]] && kill "$IMAGE_PID" 2>/dev/null || true
    [[ -n "$IMAGE_PID" ]] && wait "$IMAGE_PID" 2>/dev/null || true
    IMAGE_PID=""
}

sde_test_compositor

"$IMAGE_DECODER" > "$RUNTIME/imagedecoder.log" 2>&1 &
IMAGE_PID=$!
# Wait for listen(), not just the socket file: LibWM probes the path and falls
# back in-process if nothing is accepting yet.
sde_test_wait_log "$RUNTIME/imagedecoder.log" 'ImageDecoder: listening on' 80 || {
    echo "SKIP: the ImageDecoder service did not start"
    exit 0
}

status=0
env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SDE_IMAGEDECODER_LOG="$RUNTIME/imagedecoder.log" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: thumbnails decode asynchronously and are deduplicated' \
        "thumbnails decode asynchronously and are deduplicated"
else
    echo "  FAIL: filemodel-thumbnail-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi

sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
