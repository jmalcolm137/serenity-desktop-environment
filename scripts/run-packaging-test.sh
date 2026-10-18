#!/usr/bin/env bash
# ci-tier: full
# Validate the AUR packaging metadata without building a package.
#
# `makepkg --printsrcinfo` parses a PKGBUILD the way makepkg will: it is the cheap
# way to keep the package names, versions, sources and dependency lists honest.
# The actual package build (SerenityOS + the framework) is a machine/network job,
# not a test; the installed layout it produces is covered by
# run-install-layout-test.sh.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v makepkg >/dev/null 2>&1; then
    echo "SKIP: makepkg is not installed"
    exit 0
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-packaging.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

found=0
for dir in "$SDE_ROOT"/packaging/aur/*/; do
    [[ -f "$dir/PKGBUILD" ]] || continue
    found=1
    name="$(basename "$dir")"
    echo "=== $name ==="
    check "$name: PKGBUILD parses" "bash -n '$dir/PKGBUILD'"
    (cd "$dir" && makepkg --printsrcinfo > "$RUNTIME/$name.srcinfo" 2> "$RUNTIME/$name.err") || {
        echo "  FAIL: makepkg --printsrcinfo"
        sed 's/^/    /' "$RUNTIME/$name.err" | head -10
        fail=1
        continue
    }
    check "$name: package name" "grep -q '^pkgname = ' '$RUNTIME/$name.srcinfo'"
    check "$name: version" "grep -q '^[[:space:]]*pkgver = ' '$RUNTIME/$name.srcinfo'"
    check "$name: architecture" "grep -q '^[[:space:]]*arch = ' '$RUNTIME/$name.srcinfo'"
    check "$name: license" "grep -q '^[[:space:]]*license = ' '$RUNTIME/$name.srcinfo'"
    check "$name: source" "grep -q '^[[:space:]]*source = ' '$RUNTIME/$name.srcinfo'"
    check "$name: checksums" "grep -q '^[[:space:]]*sha256sums = ' '$RUNTIME/$name.srcinfo'"
done

if [[ "$found" -eq 0 ]]; then
    echo "FAIL: no packaging/aur/*/PKGBUILD found"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> packaging test FAILED"
    exit 1
fi
echo "==> packaging test PASSED"
