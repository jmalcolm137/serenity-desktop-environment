#!/usr/bin/env bash
# Validate the SDE patch queue in patches/.
#
# Checks (always run, no SerenityOS checkout required):
#   1. Every patches/*.patch follows NNNN-slug.patch with a unique 4-digit prefix.
#   2. Every patch is non-empty and parses cleanly with `git apply --stat`.
#   3. Every patch on disk is documented in patches/README.md, and every
#      `*.patch` link in patches/README.md points to an existing file.
#
# Optional dry-run against a SerenityOS checkout (read-only, never modifies the tree):
#   scripts/check-patches.sh --apply-check [--tree <serenity-src>]
#
# Usage:
#   scripts/check-patches.sh [--apply-check] [--tree <serenity-src>]
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$SDE_ROOT/patches"
README="$PATCH_DIR/README.md"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
APPLY_CHECK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply-check) APPLY_CHECK=1; shift ;;
        --tree) SERENITY_SRC="${2:?--tree needs a path}"; APPLY_CHECK=1; shift 2 ;;
        -h|--help)
            sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

errors=0
patch_count=0
declare -A seen_num=()
declare -A disk_patches=()

for patch in "$PATCH_DIR"/*.patch; do
    [[ -e "$patch" ]] || continue
    name="$(basename "$patch")"
    patch_count=$((patch_count + 1))
    disk_patches["$name"]=1

    # 1. Naming convention & unique 4-digit sequence number.
    if [[ ! "$name" =~ ^([0-9]{4})-[a-z0-9][a-z0-9._-]*\.patch$ ]]; then
        echo "ERROR: bad patch filename (expected NNNN-slug.patch): $name" >&2
        errors=$((errors + 1))
    else
        num="${BASH_REMATCH[1]}"
        if [[ -n "${seen_num[$num]:-}" ]]; then
            echo "ERROR: duplicate patch number $num: ${seen_num[$num]} and $name" >&2
            errors=$((errors + 1))
        else
            seen_num["$num"]="$name"
        fi
    fi

    # 2. Non-empty & valid unified diff.
    if [[ ! -s "$patch" ]]; then
        echo "ERROR: empty patch file: $name" >&2
        errors=$((errors + 1))
    elif ! git apply --stat "$patch" >/dev/null 2>&1; then
        echo "ERROR: patch failed 'git apply --stat': $name" >&2
        errors=$((errors + 1))
    fi
done

# 3. Cross-check patches/README.md links against patches/ on disk.
if [[ ! -f "$README" ]]; then
    echo "ERROR: missing $README" >&2
    errors=$((errors + 1))
else
    declare -A readme_patches=()
    while IFS= read -r linked; do
        [[ -n "$linked" ]] || continue
        readme_patches["$linked"]=1
        if [[ -z "${disk_patches[$linked]:-}" ]]; then
            echo "ERROR: patches/README.md links to missing patch: $linked" >&2
            errors=$((errors + 1))
        fi
    done < <(grep -oE '\([0-9]{4}-[^)]+\.patch\)' "$README" | tr -d '()' | sort -u)

    for name in "${!disk_patches[@]}"; do
        if [[ -z "${readme_patches[$name]:-}" ]]; then
            echo "ERROR: patch not documented in patches/README.md: $name" >&2
            errors=$((errors + 1))
        fi
    done
fi

echo "==> patch metadata: $patch_count patches checked"

# 4. Optional read-only apply check against a SerenityOS checkout.
if [[ "$APPLY_CHECK" == 1 ]]; then
    if [[ ! -d "$SERENITY_SRC/.git" ]]; then
        echo "ERROR: SerenityOS checkout not found at $SERENITY_SRC" >&2
        exit 1
    fi

    applicable=0
    already=0
    unverifiable=0
    for patch in "$PATCH_DIR"/*.patch; do
        [[ -e "$patch" ]] || continue
        name="$(basename "$patch")"
        if git -C "$SERENITY_SRC" apply -C1 --reverse --check "$patch" >/dev/null 2>&1; then
            already=$((already + 1))
        elif git -C "$SERENITY_SRC" apply -C1 --check "$patch" >/dev/null 2>&1; then
            applicable=$((applicable + 1))
        else
            echo "ERROR: unverifiable against $SERENITY_SRC: $name" >&2
            unverifiable=$((unverifiable + 1))
            errors=$((errors + 1))
        fi
    done
    echo "==> dry-run ($SERENITY_SRC): $applicable applicable, $already already applied, $unverifiable unverifiable"
fi

if [[ "$errors" -gt 0 ]]; then
    echo "FAILED: $errors patch queue issue(s) found." >&2
    exit 1
fi
echo "OK: patch queue is clean."
