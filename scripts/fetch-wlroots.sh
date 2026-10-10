#!/usr/bin/env bash
# Fetch and build the pinned wlroots used by serenity-window-server.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLROOTS_VERSION="${WLROOTS_VERSION:-0.20.2}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -d "$WLROOTS_SRC/.git" ]]; then
    mkdir -p "$(dirname "$WLROOTS_SRC")"
    git clone --depth 1 --branch "$WLROOTS_VERSION" \
        https://gitlab.freedesktop.org/wlroots/wlroots.git "$WLROOTS_SRC"
fi

# wlroots' public headers are written in C and use two constructs that a C++
# consumer cannot parse:
#   * C99 array parameters: `void f(const float x[static 4])`
#   * an identifier named `namespace` in the wlr-layer-shell protocol
# Neutralize both (inert for the generated API) so serenity-window-server can be
# written in Serenity-style C++. Applied idempotently.
find "$WLROOTS_SRC/include" -name '*.h' -print0 \
    | xargs -0 sed -i -E 's/\[static[[:space:]]+(restrict[[:space:]]+)?([^]]+)\]/[\2]/g'
sed -i 's/char \*namespace;/char *layer_namespace;/' "$WLROOTS_SRC/include/wlr/types/wlr_layer_shell_v1.h"
sed -i -E 's/\bnamespace\b/layer_namespace/g' "$WLROOTS_SRC/types/wlr_layer_shell_v1.c"
sed -i 's/name="namespace"/name="layer_namespace"/' "$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml"

if [[ ! -f "$WLROOTS_SRC/build/build.ninja" ]]; then
    meson setup "$WLROOTS_SRC/build" "$WLROOTS_SRC" \
        --prefix="$WLROOTS_SRC/install" -Dexamples=false
fi

ninja -C "$WLROOTS_SRC/build" -j"$JOBS"
ninja -C "$WLROOTS_SRC/build" install
echo "==> wlroots $WLROOTS_VERSION installed in $WLROOTS_SRC/install"
