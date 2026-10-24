#!/usr/bin/env bash
# Build WLCS and SDE's WLCS integration module.
#
# WLCS is an external project (canonical/wlcs), fetched at a pinned revision by
# scripts/fetch-wlcs.sh. Its build lives in build/wlcs, alongside the SDE's own
# build tree, so no source or generated object from it is checked in.
#
# WLCS needs the Boost headers. Arch's `boost-libs` package provides only the
# runtime libraries, so when the development headers are absent this script
# generates the small subset WLCS uses (throw_exception and current_function) in
# the build directory. Install `boost` to use the real headers instead.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLCS_SRC="${WLCS_SRC:-$SDE_ROOT/third_party/wlcs}"
WLCS_BUILD_DIR="${WLCS_BUILD_DIR:-$SDE_ROOT/build/wlcs}"
WLROOTS_SRC="${WLROOTS_SRC:-${SDE_WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}}"
WLROOTS_VERSION="${WLROOTS_VERSION:-0.20.2}"
PROTOCOL_DIR="$WLCS_BUILD_DIR/protocol"
JOBS="${JOBS:-$(nproc)}"
CXX="${CXX:-c++}"
CC="${CC:-cc}"

"$SDE_ROOT/scripts/fetch-wlcs.sh"

mkdir -p "$WLCS_BUILD_DIR" "$PROTOCOL_DIR"

boost_args=()
if [[ -n "${WLCS_BOOST_INCLUDE_DIR:-}" ]]; then
    boost_args+=("-DBoost_INCLUDE_DIR=$WLCS_BOOST_INCLUDE_DIR")
elif [[ ! -f /usr/include/boost/version.hpp ]]; then
    stub="$WLCS_BUILD_DIR/boost-stub"
    mkdir -p "$stub/boost/exception"
    cat > "$stub/boost/version.hpp" <<'EOF'
#pragma once
#define BOOST_VERSION 109200
#define BOOST_LIB_VERSION "1_92"
EOF
    cat > "$stub/boost/throw_exception.hpp" <<'EOF'
#pragma once
#include <exception>
#include <cstring>
#include <cerrno>
#include <string>
#include <stdexcept>
#ifndef BOOST_THROW_EXCEPTION
#define BOOST_THROW_EXCEPTION(x) do { throw (x); } while (false)
#endif
EOF
    cat > "$stub/boost/current_function.hpp" <<'EOF'
#pragma once
#include <unistd.h>
#include <sys/syscall.h>
#ifndef BOOST_CURRENT_FUNCTION
#define BOOST_CURRENT_FUNCTION __builtin_FUNCTION()
#endif
EOF
    : > "$stub/boost/exception/info.hpp"
    boost_args+=("-DBoost_INCLUDE_DIR=$stub")
    # helpers.cpp relies on the include chain of real Boost headers for these
    # POSIX declarations. Force-include them with the compatibility header.
    boost_args+=("-DCMAKE_CXX_FLAGS=-include unistd.h -include sys/syscall.h")
fi

cmake -S "$WLCS_SRC" -B "$WLCS_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWLCS_BUILD_ASAN=OFF \
    -DWLCS_BUILD_TSAN=OFF \
    -DWLCS_BUILD_UBSAN=OFF \
    "${boost_args[@]}"
cmake --build "$WLCS_BUILD_DIR" -j "$JOBS"

# The integration module uses client-side generated protocol code. The full-suite
# checkout is built against the system wlroots package and need not contain a
# source checkout, so fetch the pinned source for its protocol XMLs when needed.
if [[ ! -f "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml" ]]; then
    mkdir -p "$(dirname "$WLROOTS_SRC")"
    git clone --depth 1 --branch "$WLROOTS_VERSION" \
        https://gitlab.freedesktop.org/wlroots/wlroots.git "$WLROOTS_SRC"
fi

command -v wayland-scanner >/dev/null || {
    echo "build-wlcs: wayland-scanner is required" >&2
    exit 1
}

# Keep this list explicit. A shell glob here used to turn a failed generation
# into a confusing `cc: Could not open input file` in the full-suite job.
protocol_specs=(
    "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
    "$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
    "$SDE_ROOT/Services/WindowServer/protocol/serenity-virtual-input.xml"
    "$SDE_ROOT/Services/WindowServer/protocol/serenity-wlcs-control.xml"
)
protocol_objects=()
for xml in "${protocol_specs[@]}"; do
    [[ -f "$xml" ]] || {
        echo "build-wlcs: missing protocol XML: $xml" >&2
        exit 1
    }
    name="$(basename "$xml" .xml)"
    header="$PROTOCOL_DIR/$name-client-protocol.h"
    source="$PROTOCOL_DIR/$name-protocol.c"
    object="$PROTOCOL_DIR/$name-protocol.o"
    wayland-scanner client-header "$xml" "$header"
    wayland-scanner private-code "$xml" "$source"
    [[ -s "$header" && -s "$source" ]] || {
        echo "build-wlcs: wayland-scanner did not generate $name" >&2
        exit 1
    }
    "$CC" -std=c11 -fPIC -O2 -c "$source" -o "$object"
    protocol_objects+=("$object")
done

"$CXX" -std=c++20 -shared -fPIC -O2 -g \
    -I"$WLCS_SRC/include" -I"$PROTOCOL_DIR" \
    -o "$WLCS_BUILD_DIR/sde_wlcs_integration.so" \
    "$SDE_ROOT/Tests/wlcs/sde_wlcs_integration.cpp" \
    "${protocol_objects[@]}" \
    -lwayland-client -lxkbcommon

echo "==> WLCS and SDE integration built in $WLCS_BUILD_DIR"
