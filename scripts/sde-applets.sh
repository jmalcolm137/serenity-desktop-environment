#!/usr/bin/env bash
# The SerenityOS taskbar applets are part of SDE (not libgui-wayland): SDE builds
# them (scripts/build-applets.sh) from the upstream sources, with Linux
# replacements for the SerenityOS kernel interfaces where needed.
#
# sde_applet_startup_commands <sde-root> prints the shell fragment that launches
# them, to splice into the compositor's startup command. The set and the
# arguments mirror SerenityOS's Base/etc/SystemServerUser.ini. The Audio applet is
# the AudioManager (mixer) client; the SDE AudioServer service serves that
# protocol over the audiomanager portal (see the AudioServer/ service).
sde_applet_startup_commands() {
    local root="$1"
    # Optional wrapper to prefix each command with (scripts/sde-supervise-applet.sh
    # keeps an applet running without any applet knowing about it).
    local supervise="${2:-}"
    local prefix=""
    [[ -n "$supervise" ]] && prefix="$supervise "
    local applets="${SDE_APPLET_DIR:-$root/build/Applets}"

    local cmds=()
    [[ -x "$applets/WorkspacePicker" ]] && cmds+=("${prefix}$applets/WorkspacePicker")
    [[ -x "$applets/ResourceGraph" ]] && cmds+=("${prefix}$applets/ResourceGraph --cpu=CPUGraph,#00bb00 --memory=MemoryGraph,#00bbbb --network=NetworkGraph,#bbbb00")
    [[ -x "$applets/Network" ]] && cmds+=("${prefix}$applets/Network --name=Network")
    [[ -x "$applets/ClipboardHistory" ]] && cmds+=("${prefix}$applets/ClipboardHistory")
    [[ -x "$applets/Audio" ]] && cmds+=("${prefix}$applets/Audio")
    [[ -x "$applets/Keymap" ]] && cmds+=("${prefix}$applets/Keymap")

    local out=""
    local cmd
    for cmd in "${cmds[@]}"; do
        [[ -z "$out" ]] || out="$out & "
        out="$out$cmd"
    done
    printf '%s' "$out"
}
