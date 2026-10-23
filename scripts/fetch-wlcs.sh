#!/usr/bin/env bash
# Fetch the pinned WLCS checkout used by the conformance harness.
#
# WLCS is not vendored: the test harness builds it from the upstream Git
# repository at a known revision. third_party/wlcs is ignored by git, just like
# third_party/wlroots.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLCS_SRC="${WLCS_SRC:-$SDE_ROOT/third_party/wlcs}"
WLCS_REPO="${WLCS_REPO:-https://github.com/canonical/wlcs.git}"
WLCS_REV="${WLCS_REV:-f26abf01de4611bf63da97f15c743e16abcb4bf5}"

# A checkout is a shared mutable resource. Two test runs starting together must
# not race on .git/index.lock, so serialize the fetch/checkout step.
mkdir -p "$SDE_ROOT/build"
exec 9>"$SDE_ROOT/build/.wlcs-fetch.lock"
flock 9

if [[ ! -d "$WLCS_SRC/.git" ]]; then
    mkdir -p "$(dirname "$WLCS_SRC")"
    git clone --filter=blob:none "$WLCS_REPO" "$WLCS_SRC"
fi

if ! git -C "$WLCS_SRC" cat-file -e "${WLCS_REV}^{commit}" 2>/dev/null; then
    git -C "$WLCS_SRC" fetch --depth 1 origin "$WLCS_REV"
fi
git -C "$WLCS_SRC" checkout --quiet --detach "$WLCS_REV"
echo "==> WLCS $WLCS_REV checked out in $WLCS_SRC"
