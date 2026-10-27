#!/usr/bin/env bash
# The SerenityOS taskbar applets are part of SDE (not libgui-wayland): SDE builds
# them (scripts/build-applets.sh) from the upstream sources, with Linux
# replacements for the SerenityOS kernel interfaces where needed.
#
# The set and the arguments mirror SerenityOS's Base/etc/SystemServerUser.ini.
# The Audio applet is the AudioManager (mixer) client; the SDE AudioServer service
# serves that protocol over the audiomanager portal (see the
# Services/AudioServer/ service).
#
# Three views of the same list:
#
#   sde_applet_startup_commands <root> [wrapper] [battery] [wifi]
#       a shell fragment (`a & b & ...`) for tests that start the applets
#       themselves, optionally prefixing each with a wrapper;
#   sde_applet_host_command <root> <supervisor> [battery] [wifi]
#       one command that starts the supervisor with every applet, which is how
#       the session runs them (one process instead of one wrapper per applet);
#   sde_applet_specs <root> [battery] [wifi]
#       the underlying `<name>|<quoted command>` lines.

_sde_emit_applet_spec() {
    local name="$1"
    shift
    printf '%s|' "$name"
    printf '%q ' "$@"
    printf '\n'
}

# sde_applet_specs <root> [battery] [wifi]
#   Print one applet per line as "<name>|<shell-quoted command>". The optional
#   battery and Wi-Fi applet paths are appended when they exist (the session
#   passes them; tests that only want the SerenityOS applets omit them).
sde_applet_specs() {
    local root="$1"
    local battery="${2:-}"
    local wifi="${3:-}"
    local applets="${SDE_APPLET_DIR:-$root/build/Applets}"

    if [[ -x "$applets/WorkspacePicker" ]]; then
        _sde_emit_applet_spec WorkspacePicker "$applets/WorkspacePicker"
    fi
    if [[ -x "$applets/ResourceGraph" ]]; then
        _sde_emit_applet_spec ResourceGraph \
            "$applets/ResourceGraph" --cpu=CPUGraph,#00bb00 --memory=MemoryGraph,#00bbbb --network=NetworkGraph,#bbbb00
    fi
    if [[ -x "$applets/Network" ]]; then
        _sde_emit_applet_spec Network "$applets/Network" --name=Network
    fi
    if [[ -x "$applets/ClipboardHistory" ]]; then
        _sde_emit_applet_spec ClipboardHistory "$applets/ClipboardHistory"
    fi
    if [[ -x "$applets/Audio" ]]; then
        _sde_emit_applet_spec Audio "$applets/Audio"
    fi
    if [[ -x "$applets/Keymap" ]]; then
        _sde_emit_applet_spec Keymap "$applets/Keymap"
    fi

    if [[ -n "$battery" && -x "$battery" ]]; then
        _sde_emit_applet_spec BatteryApplet "$battery"
    fi
    if [[ -n "$wifi" && -x "$wifi" ]]; then
        _sde_emit_applet_spec WifiApplet "$wifi"
    fi
    return 0
}

# sde_applet_startup_commands <root> [wrapper] [battery] [wifi]
#   The applets as a single shell fragment, for tests and scripts that start
#   them directly rather than through the session's supervisor.
sde_applet_startup_commands() {
    local root="$1"
    local supervise="${2:-}"
    local battery="${3:-}"
    local wifi="${4:-}"
    local prefix=""
    if [[ -n "$supervise" ]]; then
        prefix="$supervise "
    fi

    local out=""
    local spec command
    while IFS= read -r spec; do
        [[ -n "$spec" ]] || continue
        command="${spec#*|}"
        if [[ -n "$prefix" ]]; then
            command="$prefix$command"
        fi
        if [[ -z "$out" ]]; then
            out="$command"
        else
            out="$out & $command"
        fi
    done < <(sde_applet_specs "$root" "$battery" "$wifi")
    printf '%s' "$out"
    return 0
}

# sde_applet_host_command <root> <supervisor> [battery] [wifi]
#   One command that starts <supervisor> with every applet. The supervisor
#   restarts a crashed applet and runs all the backoffs independently.
sde_applet_host_command() {
    local root="$1"
    local host="$2"
    local battery="${3:-}"
    local wifi="${4:-}"

    local out="$host"
    local spec
    while IFS= read -r spec; do
        [[ -n "$spec" ]] || continue
        out="$out --applet $(printf '%q' "$spec")"
    done < <(sde_applet_specs "$root" "$battery" "$wifi")
    printf '%s' "$out"
    return 0
}
