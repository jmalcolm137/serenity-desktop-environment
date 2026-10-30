#!/usr/bin/env bash
# ci-tier: fast
# Applet spec quoting (scripts/sde-applets.sh).
#
# The session splices `sde_applet_host_command` into the compositor's startup
# string. That string is parsed by more than one shell layer, so every spec must
# survive quoting and, in particular, must not end with an escaped trailing
# space: the last spec used to lose that space and keep a literal backslash,
# which made WifiApplet fail with status 127 on every start.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-applet-spec.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

fail=0
ok() { echo "  ok: $1"; }
missing() { echo "  MISSING: $1"; fail=1; }

APPLETS="$RUNTIME/applets"
mkdir -p "$APPLETS" "$RUNTIME/with space"
for applet in WorkspacePicker ResourceGraph Network ClipboardHistory Audio Keymap BatteryApplet; do
    printf '#!/bin/sh\nexit 0\n' > "$APPLETS/$applet"
    chmod +x "$APPLETS/$applet"
done
WIFI="$RUNTIME/with space/WifiApplet"
printf '#!/bin/sh\nexit 0\n' > "$WIFI"
chmod +x "$WIFI"

export SDE_APPLET_DIR="$APPLETS"
SPEC_LOG="$RUNTIME/specs"
cat > "$RUNTIME/fake-host" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$SDE_SPEC_LOG"
EOF
chmod +x "$RUNTIME/fake-host"

HOST_COMMAND="$(sde_applet_host_command "$RUNTIME" "$RUNTIME/fake-host" "$APPLETS/BatteryApplet" "$WIFI")"
STARTUP_COMMANDS="$(sde_applet_startup_commands "$RUNTIME" "" "$APPLETS/BatteryApplet" "$WIFI")"

echo "=== the host command is not left with an escaped trailing space ==="
if [[ "$HOST_COMMAND" == *'\ ' ]]; then
    missing "the host command ends with an escaped space"
else
    ok "the host command does not end with an escaped space"
fi
if [[ "$HOST_COMMAND" == *'\' ]]; then
    missing "the host command ends with a backslash"
else
    ok "the host command does not end with a backslash"
fi
if [[ "$STARTUP_COMMANDS" == *'\ ' || "$STARTUP_COMMANDS" == *'\' ]]; then
    missing "the startup command ends with a slash-escape"
else
    ok "the startup command does not end with a slash-escape"
fi

echo "=== every spec survives the shell and keeps its exact command ==="
SDE_SPEC_LOG="$SPEC_LOG" sh -c "true & $HOST_COMMAND"
spec="$(grep '|' "$SPEC_LOG" | tail -1)"
if [[ "${spec%%|*}" == "WifiApplet" ]]; then
    ok "the last spec is the Wi-Fi applet"
else
    missing "the last spec is the Wi-Fi applet (got '${spec%%|*}')"
fi
if [[ "$spec" == *' ' || "$spec" == *'\' ]]; then
    missing "the last spec ends in whitespace or a backslash"
else
    ok "the last spec does not end in whitespace or a backslash"
fi
spec_command="${spec#*|}"
eval "set -- $spec_command"
if [[ "${1:-}" == "$WIFI" ]]; then
    ok "the Wi-Fi command resolves to the real path"
else
    missing "the Wi-Fi command resolves to '$WIFI' (got '${1:-}')"
fi
if [[ -x "${1:-}" ]]; then
    ok "the Wi-Fi executable exists"
else
    missing "the Wi-Fi executable exists"
fi

first_spec="$(grep '|' "$SPEC_LOG" | head -1)"
if [[ "$first_spec" == *' ' || "$first_spec" == *'\' ]]; then
    missing "the first spec ends in whitespace or a backslash"
else
    ok "the first spec does not end in whitespace or a backslash"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- host command ---"
    printf '%s\n' "$HOST_COMMAND"
    echo "--- host argv ---"
    cat "$SPEC_LOG" 2>/dev/null || true
    echo "==> applet spec test FAILED"
    exit 1
fi
echo "==> applet spec test PASSED"
