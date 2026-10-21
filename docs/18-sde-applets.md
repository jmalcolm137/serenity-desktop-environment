# 18: The SDE taskbar applets

The SerenityOS taskbar applets (**WorkspacePicker, ResourceGraph, Network,
ClipboardHistory, Keymap, Audio**) are part of the **desktop environment**, not
of `libgui-wayland`: SDE builds and ships them. Where an applet reached into a
SerenityOS kernel interface, that interface is replaced with its **Linux
equivalent**, never shimmed.

## 1. Build

`Applets/CMakeLists.txt` builds each applet from the **unmodified upstream
sources** in the SerenityOS checkout (`SERENITY_SRC/Userland/Applets/<Name>/`),
linking the Serenity userland libraries (from the `libgui-wayland` build), the
in-process WindowServer shim (`windowserver`/LibWM). `Libraries/Compat/` supplies the one Serenity-only header the sources
need (`WindowServer/Window.h` → `WMEventMask`).

```sh
scripts/build-applets.sh     # -> build/Applets/{WorkspacePicker,ResourceGraph,...}
```

An applet that needs a Linux replacement for a kernel interface keeps a **local
copy** under `Applets/<Name>/`; those files take precedence, file by file, over
the upstream ones. Everything else is compiled verbatim from upstream, so a diff
against SerenityOS stays trivial.

## 2. Linux replacements for SerenityOS kernel interfaces

| Applet | Upstream reads | SDE reads |
| --- | --- | --- |
| **Network** | `/sys/kernel/net/adapters` (JSON) | `getifaddrs()` (IPv4) + `/sys/class/net/<if>/{operstate,speed}` |
| **ResourceGraph** | `/sys/kernel/stats`, `/sys/kernel/memstat`, `/sys/kernel/net/adapters` | `/proc/stat`, `/proc/meminfo`, `/sys/class/net/<if>/statistics/{rx,tx}_bytes` |

Both keep everything else (widgets, painting, tooltips, notifications) identical
to upstream; only the data source differs. `Applets/Network/main.cpp` and
`Applets/ResourceGraph/main.cpp` are the SDE copies.

The remaining applets read **no** kernel interface:

- **WorkspacePicker** talks to the WindowManagerServer (workspace grid/switch).
- **ClipboardHistory** uses the LibGUI clipboard.
- **Keymap** uses `Keyboard::CharacterMap` (LibKeyboard).
- **Audio** uses `Audio::ConnectionToManagerServer` (LibAudio).

When an applet **launches a Serenity application** (ResourceGraph opens
`/bin/SystemMonitor` on click), the path resolves through the port's
`Process::resolve_executable_path()`: the session sets **`SDE_APP_BIN_DIR`** to the
port's bin directory, so `/bin/SystemMonitor` reaches it even though the applet
itself lives in `build/Applets` (libgui-wayland patch `0011`). Covered by
`scripts/run-applet-launch-test.sh`.

## 3. Library abstractions stay in the libraries (the port)

Where an applet only needs a *library* abstraction, that abstraction lives in the
library (which is not desktop-environment-specific), and the applet stays
unmodified:

- **LibKeyboard**: the active keymap belongs to the display server on Linux, not
  the kernel. `CharacterMap::fetch_system_map()` now reads the standard
  `XKB_DEFAULT_LAYOUT` variable (first entry = active layout; `us` fallback) on
  non-Serenity hosts, and `set_system_map()` is a no-op (the layout is changed
  through the display server's own protocol). Port patch
  `0033-libkeyboard-linux-system-map.patch`. The SDE compositor exports
  `XKB_DEFAULT_LAYOUT`/`XKB_DEFAULT_VARIANT` to its children from `[Keyboard]`.
- **LibAudio / AudioServer**: the Audio applet's mixer protocol
  (`/tmp/session/%sid/portal/audiomanager`) is served by the SDE **`AudioServer`
  service** (spawned on demand by LibWM), backed by the shared `Mixer` (main
  volume/mute applied to the mixed output) and the PipeWire backend. The applet
  is an unmodified Serenity client and reaches it over the portal; no shim is
  linked into it.

## 4. Session integration

The session startup launches the applets (with the same arguments as SerenityOS's
`Base/etc/SystemServerUser.ini`) via `scripts/sde-applets.sh`
(`sde_applet_startup_commands`), spliced into the compositor's startup command by
`scripts/start-sde-session.sh` and `scripts/start-sde-nested.sh`. They are built
on demand, like the SDE-owned `BatteryApplet` (docs/16) and `WifiApplet`
(docs/19), which the same scripts also start.

## 5. Compositor: applet re-layout on resize

An applet may size itself **after** `show()` (the Audio applet resizes to
`16x16`/`44x16` once it knows whether to show the percentage). `layer_surface_commit`
now also re-arranges when the **committed** surface size changes, not only the
desired size; otherwise the applet area was computed from the applet's initial
(full-output) size and never corrected. See docs/04-known-gaps.md.

## 6. Test

`scripts/run-sde-applets-test.sh` runs every applet under the headless
compositor and asserts each maps as its `serenity-applet:<name>` layer surface,
that the Audio applet settles at `16x16`, and that none abort or log a failed
Linux data read.
