#!/usr/bin/env bash
# ci-tier: fast
# Phase F1 freedesktop.org compliance tests:
#   - XDG Base Directory Specification (LibXDG/BaseDirs)
#   - XDG Desktop Application Autostart Specification (LibXDG/Autostart + sde-autostart --dry-run)
#   - Base/systemd/user/sde-session.target presence
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-target.sh" libwm-xdg-basedirs-test libwm-xdg-autostart-test sde-autostart >/dev/null

"$SDE_ROOT/build/test/libwm-xdg-basedirs-test"
"$SDE_ROOT/build/test/libwm-xdg-autostart-test"

# Smoke-test sde-autostart --dry-run against a private XDG tree.
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-xdg-f1.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

mkdir -p "$RUNTIME/cfg/autostart" "$RUNTIME/sys/autostart"
cat > "$RUNTIME/sys/autostart/demo.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Demo
Exec=/bin/true --demo %u
OnlyShowIn=SDE;
EOF

out="$(XDG_CONFIG_HOME="$RUNTIME/cfg" XDG_CONFIG_DIRS="$RUNTIME/sys" XDG_CURRENT_DESKTOP=SDE \
    "$SDE_ROOT/build/tools/sde-autostart" --dry-run)"
if ! grep -qF $'demo.desktop\t/bin/true --demo' <<<"$out"; then
    echo "FAIL: sde-autostart --dry-run output mismatch: $out" >&2
    exit 1
fi
echo "  ok: sde-autostart --dry-run resolved system autostart entry"

if [[ ! -f "$SDE_ROOT/Base/systemd/user/sde-session.target" ]]; then
    echo "FAIL: missing Base/systemd/user/sde-session.target" >&2
    exit 1
fi
echo "  ok: Base/systemd/user/sde-session.target exists"
echo "==> XDG F1 tests PASSED"
