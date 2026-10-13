# 19 — The SDE Wi-Fi applet

**Status:** Implemented · **Date:** 2026-10-12
**Related:** [`18-sde-applets.md`](18-sde-applets.md) · [`16-battery-applet.md`](16-battery-applet.md) · [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) · [`14-r4.4-config.md`](14-r4.4-config.md)

`WifiApplet` is the SDE's Wi-Fi applet for the Taskbar's applet area. It is modeled
on KDE Plasma's **Networks** applet (`plasma-nm`): a Breeze signal-arc tray icon,
a tooltip, and a popup with a Wi-Fi switch, the live network list, connect /
disconnect and a passphrase prompt. It sits in the applet area **just to the left
of the battery applet**.

It is a plain SerenityOS LibGUI application (like `BatteryApplet`), built against
the Serenity userland libraries from the `libgui-wayland` build. It is a
**standards-based citizen**: it talks to **NetworkManager over the system D-Bus**
— the same source KDE uses — with libdbus, integrated into LibCore's event loop.

## 1. Where it lives

| Path | Role |
|---|---|
| `WifiApplet/src/NetworkManagerClient.h` / `.cpp` | The NetworkManager D-Bus client (system bus, via libdbus + `Core::Notifier`) and the `org.freedesktop.NetworkManager.SecretAgent`. |
| `WifiApplet/src/WifiIcon.h` / `.cpp` | The Breeze Wi-Fi glyphs, drawn with LibGfx (no theme assets). |
| `WifiApplet/src/WifiPopup.h` / `.cpp` | The popup: toolbar, network list, inline passphrase field, hidden-network form. |
| `WifiApplet/src/WifiWidget.h` / `.cpp` | The tray widget, tooltip, context menu and popup host. |
| `WifiApplet/src/main.cpp` | `serenity_main`, the applet window and the command line. |
| `scripts/build-wifi-applet.sh` | Builds it into `build/WifiApplet/WifiApplet`. |
| `tools/mock-networkmanager.c` | A mock NetworkManager for the headless test. |
| `scripts/run-wifi-applet-test.sh` | The headless exit test. |

## 2. Why NetworkManager over D-Bus

KDE's applet is a frontend to NetworkManager, and this project's interoperability
principle (P8) says to use the standard channel rather than invent a private one.
`WifiApplet` therefore speaks the same D-Bus API as `plasma-nm`:

- **Manager** `org.freedesktop.NetworkManager` — `WirelessEnabled`,
  `WirelessHardwareEnabled`, `NetworkingEnabled`, `Connectivity`, `Devices`,
  `ActiveConnections`; `AddAndActivateConnection`, `ActivateConnection`.
- **Device** / **Device.Wireless** — the Wi-Fi device, its `AccessPoints` and
  `ActiveAccessPoint`; `Disconnect`, `RequestScan`.
- **AccessPoint** — `Ssid`, `Strength`, `Frequency`, `Flags`/`WpaFlags`/`RsnFlags`
  (security), `Mode`.
- **Settings** — `ListConnections` + `GetSettings`, to know an SSID has a saved
  profile (and to reactivate it rather than create a duplicate).
- **SecretAgent** — the applet registers as a NetworkManager **secret agent**
  (`AgentManager.Register`), so activating a *saved* WPA network prompts for the
  passphrase through us, exactly like KDE.

The D-Bus connection is driven by a `Core::Notifier` on the bus socket and
`dbus_connection_dispatch`, so it lives in the LibGUI event loop with no worker
thread. `PropertiesChanged` / `AccessPointAdded` / device-state signals only mark
the state dirty; the actual re-read happens on a **fresh turn of the event loop**
(`Core::deferred_invoke`), so we never reenter the dispatcher with a blocking call.

When there is no system bus or no NetworkManager, the applet still runs: it logs
the failure and shows the `network-unavailable` state.

## 3. The look (Breeze, drawn rather than shipped)

The glyphs are reproduced from the Breeze `network-wireless-*` icons on a 22-unit
grid — a ring dot at the bottom centre and three concentric arc-bands above it —
and the signal level fills in the bands, exactly as KDE's `ConnectionIcon` picks
`network-wireless-0/20/40/60/80/100` (`libs/connectionicon.cpp`). The level →
signal thresholds match KDE (`==0 → 0`, `<20 → 20`, … `<100 → 100`). The
disconnected state overlays the red X, the off/unavailable state the red slash,
and a connecting activation the yellow spinner. The band widths are widened to at
least one device pixel so the 16 px tray icon stays crisp.

The tray tooltip reads `Networks` with the active connection (or `Disconnected` /
`Wi-Fi is off`).

## 4. The popup

Opening the applet (left click) shows a KDE-style popup above it:

- a **toolbar**: the Wi-Fi on/off switch (icon-only), a **search** field, a
  **Hidden…** button and a **Configure** button (spawns `nm-connection-editor`);
- the **network list** — one row per SSID, strongest access point: the signal
  fan, the SSID, a subtitle (`Connected` / `Saved` / the strength), and a lock for
  secured networks. The connected network is drawn in the accent colour;
- clicking a network **connects**; clicking the connected one **disconnects**;
  a secured network with no saved profile **expands in place** to a passphrase
  field (Enter, or a click on the row, connects; Escape cancels), as in KDE;
- a **placeholder** when there is nothing to show (`Wi-Fi is deactivated`,
  `Looking for Wi-Fi networks`, `No available connections`, `No matches`).

The popup is a `GUI::WindowType::Popup` (an overlay layer surface), so it is
undecorated, out of the window list and takes keyboard focus for the search and
passphrase fields. (Keyboard focus for `on_demand` layer surfaces is provided by
the compositor; see `focus_layer_surface` in `WindowServer/src/main.cpp`.)

## 5. Running on the Taskbar

The applet declares itself with `window->set_window_type(GUI::WindowType::Applet)`
and the title `WiFi`; LibWindowServer maps that to a top-layer layer surface in
the `serenity-applet:WiFi` namespace, and the compositor's `AppletManager` lays it
out in the Taskbar's applet area.

Applet order is right-to-left by descending index, so the compositor appends
**`WiFi` then `Battery`** to `[Applet] Order` (unless the user placed them), which
puts Wi-Fi immediately to the left of the battery applet (`WindowServer/src/Config.cpp`).
The session scripts build it on demand and add it to the compositor's startup
command, next to the battery applet.

## 6. Command line

```
WifiApplet [--name <name>]
```

`--name` sets the applet name (the window title), for `[Applet] Order`; it
defaults to `WiFi`.

## 7. Exit test

`scripts/run-wifi-applet-test.sh` is hermetic: it starts a **private D-Bus**
(`dbus-daemon`), serves a **mock NetworkManager** (`tools/mock-networkmanager.c`)
with two networks (`MockNet`, a secured 82 % network that is connected, and the
open `OpenGuest`) and runs the compositor headless. It asserts:

- the applet reads the mock state (`WifiApplet: 2 networks, connected to MockNet (82%)`);
- it maps as the `serenity-applet:WiFi` layer surface;
- the applet area places Wi-Fi to the left of Battery.

## 8. Known limitations

- **No VPN / mobile-broadband parity.** KDE's applet also manages VPNs and WWAN;
  this applet currently covers Wi-Fi (and wired is out of scope for a Wi-Fi applet).
- **Connectivity is informational.** The `Connectivity`/captive-portal states are
  read but there is no "log in to this network" affordance yet.
- **Security is summarised as a lock.** The list shows a lock for any secured
  network; it does not yet distinguish WEP/WPA/WPA2/WPA3/SAE.
- **The password is always sent as a WPA-PSK.** For enterprise (802.1X) networks
  the agent path would need the full set of secrets.
- **The popup does not close on an outside click** (like the battery applet's): it
  closes on the applet click or Escape.
