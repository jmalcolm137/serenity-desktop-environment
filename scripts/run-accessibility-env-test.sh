#!/usr/bin/env bash
# Headless test: the session exports the AT-SPI bus to its clients.
#
# LibGUI publishes its accessibility tree only when AT_SPI_BUS_ADDRESS is set
# (docs/30-at-spi.md). The bus is a standard D-Bus-activated service; the session
# asks it for the address rather than starting a daemon. This test pins the
# discovery/parse rules and the "no bus, no change" paths without at-spi2-core.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
HELPER="$SDE_ROOT/scripts/sde-accessibility.sh"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-accessibility-env.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

mkdir -p "$RUNTIME/bin" "$RUNTIME/emptybin"
cat > "$RUNTIME/bin/gdbus" <<'EOF'
#!/bin/sh
echo "called" >> "${SDE_TEST_GDBUS_LOG:?}"
case "${SDE_TEST_GDBUS_REPLY:-ok}" in
    bad) printf '()\n' ;;
    *)   printf "('unix:path=/tmp/fake-atspi-bus',)\n" ;;
esac
EOF
chmod +x "$RUNTIME/bin/gdbus"
export SDE_TEST_GDBUS_LOG="$RUNTIME/gdbus.log"
export DBUS_SESSION_BUS_ADDRESS="unix:path=/tmp/fake-session-bus"

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

# Run the helper in a subshell so one case cannot leak into the next.
discover() {
    (
        set -u
        # shellcheck source=scripts/sde-accessibility.sh
        . "$HELPER"
        sde_export_at_spi_bus
        printf '%s' "${AT_SPI_BUS_ADDRESS:-}"
    )
}

echo "=== the bus address is discovered, parsed and exported ==="
: > "$SDE_TEST_GDBUS_LOG"
unset AT_SPI_BUS_ADDRESS
address="$(PATH="$RUNTIME/bin:$PATH" discover)"
check "the address is the string inside the tuple" \
    "[[ '$address' == 'unix:path=/tmp/fake-atspi-bus' ]]"
check "the discovery used the session bus service" \
    "grep -q called '$SDE_TEST_GDBUS_LOG'"
child_address="$(AT_SPI_BUS_ADDRESS="$address" PATH="$RUNTIME/bin:$PATH" sh -c 'printf %s "$AT_SPI_BUS_ADDRESS"')"
check "the address is exported to child processes" \
    "[[ '$child_address' == 'unix:path=/tmp/fake-atspi-bus' ]]"

echo "=== an existing address wins ==="
: > "$SDE_TEST_GDBUS_LOG"
address="$(AT_SPI_BUS_ADDRESS='unix:path=/already-set' PATH="$RUNTIME/bin:$PATH" discover)"
check "the caller's address is kept" "[[ '$address' == 'unix:path=/already-set' ]]"
check "the bus was not queried again" "[[ ! -s '$SDE_TEST_GDBUS_LOG' ]]"

echo "=== accessibility can be turned off ==="
: > "$SDE_TEST_GDBUS_LOG"
unset AT_SPI_BUS_ADDRESS
address="$(SDE_ACCESSIBILITY=0 PATH="$RUNTIME/bin:$PATH" discover)"
check "no address is exported" "[[ -z '$address' ]]"
check "the bus was not queried" "[[ ! -s '$SDE_TEST_GDBUS_LOG' ]]"

echo "=== a malformed reply is ignored ==="
: > "$SDE_TEST_GDBUS_LOG"
unset AT_SPI_BUS_ADDRESS
address="$(SDE_TEST_GDBUS_REPLY=bad PATH="$RUNTIME/bin:$PATH" discover)"
check "no address is exported" "[[ -z '$address' ]]"

echo "=== no gdbus is a no-op ==="
unset AT_SPI_BUS_ADDRESS
address="$(PATH="$RUNTIME/emptybin" discover)"
check "no address is exported" "[[ -z '$address' ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "==> accessibility environment test FAILED"
    exit 1
fi
echo "==> accessibility environment test PASSED"
