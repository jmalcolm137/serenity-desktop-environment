#!/usr/bin/env bash
# Publish the AT-SPI accessibility bus to the session environment.
#
# LibGUI's AccessibilityServer starts only when AT_SPI_BUS_ADDRESS is present
# (docs/30-at-spi.md). The at-spi bus is a standard D-Bus-activated service
# (`org.a11y.Bus`, provided by at-spi2-core), so a session asks it for the
# address instead of starting a daemon of its own. Without at-spi2-core, or with
# no session bus, this is a no-op and applications behave exactly as before.
#
# Usage: source this file, then call sde_export_at_spi_bus.
#
# Environment:
#   AT_SPI_BUS_ADDRESS   already set: kept (an embedding or a test wins)
#   SDE_ACCESSIBILITY=0  skip discovery (tests, minimal sessions)
#   DBUS_SESSION_BUS_ADDRESS  required; without a session bus there is no service

sde_export_at_spi_bus() {
    if [[ -n "${AT_SPI_BUS_ADDRESS:-}" ]]; then
        return 0
    fi
    if [[ "${SDE_ACCESSIBILITY:-1}" == "0" ]]; then
        return 0
    fi
    if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
        return 0
    fi
    command -v gdbus >/dev/null 2>&1 || return 0

    local reply address
    reply="$(gdbus call --session --timeout 2 \
        --dest org.a11y.Bus --object-path /org/a11y/bus \
        --method org.a11y.Bus.GetAddress 2>/dev/null)" || return 0

    # The reply is a tuple with one string: ('unix:path=/run/user/1000/at-spi/bus',)
    address="${reply#*\'}"
    address="${address%%\'*}"
    if [[ "$address" != unix:* && "$address" != tcp:* && "$address" != nonce-tcp:* && "$address" != "unix:runtime=yes"* ]]; then
        return 0
    fi

    export AT_SPI_BUS_ADDRESS="$address"
    echo "SDE: accessibility bus $AT_SPI_BUS_ADDRESS" >&2
}
