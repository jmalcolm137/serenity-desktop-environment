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
    local applets="$root/build/Applets"

    local cmds=()
    [[ -x "$applets/WorkspacePicker" ]] && cmds+=("$applets/WorkspacePicker")
    [[ -x "$applets/ResourceGraph" ]] && cmds+=("$applets/ResourceGraph --cpu=CPUGraph,#00bb00 --memory=MemoryGraph,#00bbbb --network=NetworkGraph,#bbbb00")
    [[ -x "$applets/Network" ]] && cmds+=("$applets/Network --name=Network")
    [[ -x "$applets/ClipboardHistory" ]] && cmds+=("$applets/ClipboardHistory")
    [[ -x "$applets/Audio" ]] && cmds+=("$applets/Audio")
    [[ -x "$applets/Keymap" ]] && cmds+=("$applets/Keymap")

    local out=""
    local cmd
    for cmd in "${cmds[@]}"; do
        [[ -z "$out" ]] || out="$out & "
        out="$out$cmd"
    done
    printf '%s' "$out"
}
