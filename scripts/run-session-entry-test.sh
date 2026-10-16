#!/usr/bin/env bash
# Test the display-manager session entry: session/sde.desktop.in + session/sde-session.in
# and scripts/install-session.sh (install / uninstall / dry-run), installed into a
# temp prefix so no system paths are touched. Also validates the .desktop if
# desktop-file-validate is available.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$SDE_ROOT/scripts/install-session.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-sessionentry.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT

fail=0
check() { if grep -qF "$1" <<<"$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; return 0; }

BIN="$RUNTIME/bin"
SESSIONS="$RUNTIME/wayland-sessions"

echo "== (A) templates are well-formed =="
check "Name=Serenity Desktop Environment" "$(cat "$SDE_ROOT/session/sde.desktop.in")"
check "Type=Application" "$(cat "$SDE_ROOT/session/sde.desktop.in")"
check "DesktopNames=SDE" "$(cat "$SDE_ROOT/session/sde.desktop.in")"
check "Exec=@EXEC@" "$(cat "$SDE_ROOT/session/sde.desktop.in")"
check "start-sde-session.sh" "$(cat "$SDE_ROOT/session/sde-session.in")"
bash -n "$SDE_ROOT/scripts/install-session.sh" && echo "  ok: installer syntax"

echo "== (B) dry run writes nothing =="
"$INSTALL" --repo "$SDE_ROOT" --bin-dir "$BIN" --sessions-dir "$SESSIONS" --dry-run >/dev/null
if [[ -e "$SESSIONS/sde.desktop" || -e "$BIN/sde-session" ]]; then
    echo "  FAIL: dry run wrote files"; fail=1
else
    echo "  ok: dry run wrote nothing"
fi

echo "== (C) install into a temp prefix =="
"$INSTALL" --repo "$SDE_ROOT" --bin-dir "$BIN" --sessions-dir "$SESSIONS" > "$RUNTIME/install.log" 2>&1 || { cat "$RUNTIME/install.log"; fail=1; }
cat "$RUNTIME/install.log"
entry="$(cat "$SESSIONS/sde.desktop")"
check "Exec=$BIN/sde-session" "$entry"
check "DesktopNames=SDE" "$entry"
check "Type=Application" "$entry"
if [[ -x "$BIN/sde-session" ]]; then echo "  ok: launcher is executable"; else echo "  FAIL: launcher not executable"; fail=1; fi
check "$SDE_ROOT/scripts/start-sde-session.sh" "$(cat "$BIN/sde-session")"
sh -n "$BIN/sde-session" && echo "  ok: launcher syntax"

if command -v desktop-file-validate >/dev/null 2>&1; then
    out="$(desktop-file-validate "$SESSIONS/sde.desktop" 2>&1 || true)"
    # The validator's key list predates the session "DesktopNames" key (used by
    # SDDM/GDM and by the system's own *.desktop sessions), so ignore that one.
    filtered="$(grep -v 'key "DesktopNames"' <<<"$out" || true)"
    if [[ -z "$filtered" ]]; then
        echo "  ok: desktop-file-validate"
    else
        echo "  MISSING: desktop-file-validate: $filtered"
        fail=1
    fi
else
    echo "  SKIP: desktop-file-validate not installed"
fi

echo "== (D) uninstall removes both =="
"$INSTALL" --repo "$SDE_ROOT" --bin-dir "$BIN" --sessions-dir "$SESSIONS" --uninstall >/dev/null
if [[ -e "$SESSIONS/sde.desktop" || -e "$BIN/sde-session" ]]; then
    echo "  FAIL: uninstall left files"; fail=1
else
    echo "  ok: both removed"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> session-entry test FAILED"
    exit 1
fi
echo "==> session-entry test PASSED"
