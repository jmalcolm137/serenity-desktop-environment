#!/usr/bin/env bash
# ci-tier: fast
# Phase F4 freedesktop.org compliance tests:
#   - LibXDG Trash unit tests ($XDG_DATA_HOME/Trash, .trashinfo, collision, restore, empty)
#   - sde-trash CLI end-to-end (trash -> list -> restore -> empty)
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-target.sh" libwm-xdg-trash-test sde-trash >/dev/null
"$SDE_ROOT/build/test/libwm-xdg-trash-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-xdg-f4.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

mkdir -p "$RUNTIME/data" "$RUNTIME/work"
printf 'hello trash\n' > "$RUNTIME/work/note.txt"

XDG_DATA_HOME="$RUNTIME/data" "$SDE_ROOT/build/tools/sde-trash" trash "$RUNTIME/work/note.txt" >/dev/null
[[ ! -e "$RUNTIME/work/note.txt" ]]
list_out="$(XDG_DATA_HOME="$RUNTIME/data" "$SDE_ROOT/build/tools/sde-trash" list)"
grep -qF "note.txt" <<<"$list_out"
XDG_DATA_HOME="$RUNTIME/data" "$SDE_ROOT/build/tools/sde-trash" restore "note.txt"
[[ -f "$RUNTIME/work/note.txt" ]]
echo "  ok: sde-trash CLI trash/list/restore round-trip succeeded"

echo "==> XDG F4 tests PASSED"
