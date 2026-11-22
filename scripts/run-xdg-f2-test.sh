#!/usr/bin/env bash
# ci-tier: fast
# Phase F2 freedesktop.org compliance tests:
#   - LibXDG MimeDatabase, MimeApps & DesktopEntry unit tests
#   - sde-open --dry-run resolution for files and URIs
#   - generate-desktop-entries.sh cross-desktop output (serenity-* prefix,
#     host Exec/TryExec via sde-app, OnlyShowIn only for SDE-only settings)
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"

"$SDE_ROOT/scripts/build-target.sh" libwm-xdg-mime-test sde-open >/dev/null

"$SDE_ROOT/build/test/libwm-xdg-mime-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-xdg-f2.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# 1. Test sde-open --dry-run against private XDG tree.
mkdir -p "$RUNTIME/cfg" "$RUNTIME/data/applications"
cat > "$RUNTIME/data/applications/demo-browser.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=DemoBrowser
Exec=/bin/true --open %u
MimeType=x-scheme-handler/https;
EOF
cat > "$RUNTIME/cfg/mimeapps.list" <<'EOF'
[Default Applications]
x-scheme-handler/https=demo-browser.desktop;
EOF

open_out="$(XDG_CONFIG_HOME="$RUNTIME/cfg" XDG_CONFIG_DIRS="$RUNTIME/cfg" \
    XDG_DATA_HOME="$RUNTIME/data" XDG_DATA_DIRS="$RUNTIME/data" XDG_CURRENT_DESKTOP=SDE \
    "$SDE_ROOT/build/tools/sde-open" --dry-run https://serenityos.org)"
if ! grep -qF $'x-scheme-handler/https\tdemo-browser.desktop\t/bin/true --open https://serenityos.org' <<<"$open_out"; then
    echo "FAIL: sde-open --dry-run mismatch: $open_out" >&2
    exit 1
fi
echo "  ok: sde-open --dry-run resolved https URI via mimeapps.list"

# 2. Test generate-desktop-entries.sh cross-desktop mode.
mkdir -p "$RUNTIME/af" "$RUNTIME/out"
cat > "$RUNTIME/af/Calculator.af" <<'EOF'
[App]
Name=Calculator
Executable=/bin/Calculator
Category=Utilities
FileTypes=txt
EOF
cat > "$RUNTIME/af/DisplaySettings.af" <<'EOF'
[App]
Name=Display Settings
Executable=/bin/DisplaySettings
Category=Settings
EOF

"$SDE_ROOT/scripts/generate-desktop-entries.sh" \
    --source "$RUNTIME/af" \
    --output "$RUNTIME/out" \
    --bin-dir /opt/sde/bin \
    --launcher /opt/sde/bin/sde-app \
    --id-prefix serenity- >/dev/null

calc_desktop="$RUNTIME/out/applications/serenity-Calculator.desktop"
disp_desktop="$RUNTIME/out/applications/serenity-DisplaySettings.desktop"

grep -qF 'Exec=/opt/sde/bin/sde-app Calculator' "$calc_desktop"
grep -qF 'TryExec=/opt/sde/bin/Calculator' "$calc_desktop"
grep -qF 'MimeType=text/plain;' "$calc_desktop"
if grep -qF 'OnlyShowIn=' "$calc_desktop"; then
    echo "FAIL: Calculator.desktop should not have OnlyShowIn=SDE;" >&2
    exit 1
fi
grep -qF 'OnlyShowIn=SDE;' "$disp_desktop"
echo "  ok: generate-desktop-entries.sh produces cross-desktop entries with OnlyShowIn=SDE only for SDE settings"

echo "==> XDG F2 tests PASSED"
