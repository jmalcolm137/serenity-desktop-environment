#!/usr/bin/env bash
# Print the Lagom libraries the SDE needs built, as Ninja target names.
#
# The SDE's CMake projects configure with SDE_ROOT and resolve their Lagom
# dependencies with find_library, which runs at *configure* time — so every one of
# them must already exist before the SDE configures at all. The full framework
# target list is therefore not just the compositor's dependencies: it includes the
# sets the services and applications link (LibJS/LibWeb for WebContent,
# LibTLS/LibHTTP for RequestServer, LibSQL for SQLServer, LibKeyboard for the
# KeyboardSettings applet, and so on).
#
# Derived from the `foreach(_lib ...)` loops in the SDE's CMakeLists:
#
#   grep -rhoE "foreach\(_lib [a-z ]+\)" --include=CMakeLists.txt .
#
# A target that does not exist is a hard failure the first time this runs against
# a Lagom tree (`ninja: error: unknown target '...'`), which is the right failure
# mode — but it also means this list has to be re-derived when a CMakeLists gains
# a library.
set -euo pipefail

TARGETS=(
    # The common prefix of almost every loop.
    AK LibCoreMinimal LibCoreBasic LibCore LibIPC LibThreading LibMain
    # Frameworks, by the loops that name them.
    LibGfx LibGUI LibUnicode LibCompress LibCrypto LibRIFF LibTextCodec
    LibFileSystem LibURL LibConfig LibKeyboard LibDesktop LibLaunch
    LibImageDecoderClient LibAudio LibLocale
    # Services and applications.
    LibJS LibWeb LibWebView LibTLS LibHTTP LibGemini LibWebSocket LibSQL
    # The compositor side: the shim and the GPU backend.
    windowserver eglgpu
)

printf '%s\n' "${TARGETS[@]}"
