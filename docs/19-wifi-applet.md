# 19: The SDE Wi-Fi applet

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
(the same source KDE uses) with libdbus, integrated into LibCore's event loop.

## 1. Where it lives

| Path | Role |
|---|---|
| `Applets/WiFi/src/NetworkManagerClient.h` / `.cpp` | The NetworkManager D-Bus client (system bus, libdbus on LibCore's event loop, every call asynchronous) and the `org.freedesktop.NetworkManager.SecretAgent`. |
| `Applets/WiFi/src/WifiIcon.h` / `.cpp` | The Breeze Wi-Fi glyphs, drawn with LibGfx (no theme assets). |
| `Applets/WiFi/src/WifiPopup.h` / `.cpp` | The popup: toolbar, network list, inline passphrase field, hidden-network form. |
| `Applets/WiFi/src/WifiWidget.h` / `.cpp` | The tray widget, tooltip, context menu and popup host. |
| `Applets/WiFi/src/main.cpp` | `serenity_main`, the applet window and the command line. |
| `scripts/build-wifi-applet.sh` | Builds it into `build/WifiApplet/WifiApplet`. |
| `Tools/mock-networkmanager.c` | A stateful mock NetworkManager, with fault injection, for the tests. |
| `scripts/run-wifi-applet-test.sh` | The headless exit test (the real applet in the compositor). |
| `Tests/WifiClientTest.cpp`, `scripts/run-wifi-client-test.sh` | The client's robustness test (no UI). |

## 2. Why NetworkManager over D-Bus

KDE's applet is a frontend to NetworkManager, and this project's interoperability
principle (P8) says to use the standard channel rather than invent a private one.
`WifiApplet` therefore speaks the same D-Bus API as `plasma-nm`:

- **Manager** `org.freedesktop.NetworkManager`: `WirelessEnabled`,
  `WirelessHardwareEnabled`, `NetworkingEnabled`, `Connectivity`, `Devices`,
  `ActiveConnections`; `AddAndActivateConnection`, `ActivateConnection`.
- **Device** / **Device.Wireless**: the Wi-Fi device, its `AccessPoints` and
  `ActiveAccessPoint`; `Disconnect`, `RequestScan`.
- **AccessPoint**: `Ssid`, `Strength`, `Frequency`, `Flags`/`WpaFlags`/`RsnFlags`
  (security), `Mode`.
- **Settings**: `ListConnections` + `GetSettings`, to know an SSID has a saved
  profile (and to reactivate it rather than create a duplicate).
- **SecretAgent**: the applet registers as a NetworkManager **secret agent**
  (`AgentManager.Register`), so activating a *saved* WPA network prompts for the
  passphrase through us, exactly like KDE.

### Nothing waits on the bus

The applet has one thread, and it paints the tray icon and the popup. A
synchronous D-Bus call would freeze both for as long as NetworkManager took to
answer (up to the call timeout for a stuck one), so the client never makes one:

- **Event-loop integration.** The connection's libdbus *watches* become
  `Core::Notifier`s, its *timeouts* become `Core::Timer`s, and its dispatch status
  schedules a dispatch on a fresh turn of the loop, at most 64 messages per turn so
  a flood of signals cannot starve painting. Call timeouts therefore fire on their
  own, without anything blocking to notice them.
- **Every call is asynchronous**, through one `call(message, handler)` helper:
  the handler runs exactly once, on the event loop, with the reply or with
  `nullptr` (an error, the 2 s timeout, a lost connection). Outstanding calls are
  tracked and cancelled when the connection is torn down, so a reply can never
  reach a destroyed client. Connecting is asynchronous too: a private connection
  is opened and `Hello` is sent like any other call.
- **A refresh is a parallel fan-out**: the manager's properties and the saved
  connections at once, then every device, then every Wi-Fi device's access
  points, each call filling its own slot. The last call to answer assembles the
  list (one row per SSID: the connected access point, else the strongest), sorts
  it (connected, then strength, then name) and publishes it if it changed. A call
  that fails or times out just leaves its slot empty, so one stuck access point
  costs its own row, not the whole refresh.
- **Refreshes coalesce.** Signals only ask for a refresh. One asked for while a
  refresh is in flight queues exactly one more, so the latest state always wins
  and a busy NetworkManager cannot keep restarting the refresh forever.
- **Actions** (`SetWirelessEnabled`, `ActivateConnection`,
  `AddAndActivateConnection`, `Disconnect`, `RequestScan`) are calls whose reply
  only triggers a refresh: their effect is read back from NetworkManager.

### Recovery

- **NetworkManager stops or restarts.** The client watches its name owner
  (`NameOwnerChanged`). When it goes, the applet shows it as unavailable with no
  stale networks, and drops any passphrase prompt it had asked for. When it comes
  back, the applet registers its secret agent again (NetworkManager forgets its
  agents) and refreshes.
- **The bus goes away.** libdbus's `Disconnected` signal tears the connection down
  (on a later turn of the loop, never inside its own dispatch), and the client
  retries every 2 s until a bus answers. The same retry covers a bus that is not up
  yet when the session starts.
- **A crashed applet** is restarted by the session's applet supervisor
  (`scripts/sde-supervise-applet.sh`), which runs one process for every applet.

### The secret agent

- Only NetworkManager's current name owner may ask for secrets; anyone else gets
  `NotAuthorized`.
- The applet stores no secrets, so it only asks the user when NetworkManager
  allows interaction (`ALLOW_INTERACTION`); otherwise, as during a background
  autoconnect, it answers `NoSecrets` instead of popping a prompt up.
- A cancelled prompt answers `UserCanceled`. A newer request replaces an older
  one, which gets `AgentCanceled`. `CancelGetSecrets` closes the prompt. A prompt
  answered after its request was withdrawn is ignored.

When there is no system bus or no NetworkManager, the applet still runs and shows
the `network-unavailable` state.

## 3. The look (Breeze, drawn rather than shipped)

The glyphs are reproduced from the Breeze `network-wireless-*` icons on a 22-unit
grid (a ring dot at the bottom centre and three concentric arc-bands above it),
and the signal level fills in the bands, exactly as KDE's `ConnectionIcon` picks
`network-wireless-0/20/40/60/80/100` (`libs/connectionicon.cpp`). The level →
signal thresholds match KDE (`==0 → 0`, `<20 → 20`, … `<100 → 100`). The
disconnected state overlays the red X, the off/unavailable state the red slash,
and a connecting activation the yellow spinner. The band widths are widened to at
least one device pixel so the 16 px tray icon stays crisp.

The tray tooltip reads `Networks` with the active connection (or `Disconnected` /
`Wi-Fi is off`).

## 4. The popup

Opening the applet (left click) shows the popup above it. It is a native
SerenityOS panel rather than a copy of Plasma's, laid out on one grid (8 px
margins, 6 px spacing, 24 px controls):

```
Wi-Fi                                  [x] Enabled
Connected to MockNet
--------------------------------------------------
| ((o  MockNet                     Connected  #  |
| ((o  ScanFound                              #  |
| ((o  OpenGuest                              #  |
--------------------------------------------------
[Hidden Network...]            [Network Settings...]
```

- **Header**: the title, a one-line status (`Connected to …`, `Connecting to …`,
  `Could not connect to …` in red, `Not connected`, `Off`, `Unavailable`), and
  the radio's **Enabled** switch, disabled when NetworkManager, networking or the
  hardware switch makes it meaningless.
- **The list** sits in a sunken, white container like any SerenityOS list. Each
  row is one line in fixed columns: the signal glyph, the SSID (bold when
  connected, elided when long), a status column (`Connected`, `Connecting…`,
  `Failed`, `Saved`) and a lock column. Both columns line up down the list
  whether or not a row uses them. The list grows to 8 rows and then scrolls; a
  search field appears above it when there are more than 8 networks.
- **Opening a row**: clicking a row selects it and opens it in place, one row at
  a time. An open row shows `WPA2 · 2.4 GHz · 82% signal` and a **Connect** or
  **Disconnect** button. A network that needs a password or WEP key and has no
  saved profile, or one NetworkManager's secret agent is asking about, shows a
  masked field (with a reveal button) instead of the details; Enter connects,
  Escape cancels. A refresh while typing keeps the field, its text and its focus.
  An 802.1X (Enterprise) network without a profile says to set it up in Network
  Settings rather than offering a password field that could not work.
- **Saved networks** get a third line in an open row:

  ```
  ((o  MockNet                          Connected  #
       WPA2 · 2.4 GHz · 82% signal       [Disconnect]
       [x] Connect automatically         [  Forget  ]
  ```

  **Connect automatically** is how to stop NetworkManager rejoining a network
  without losing it: it sets the profile's `connection.autoconnect`, and the
  profile and its password stay, so one click still connects. (NetworkManager has
  no call that sets a single property, so the applet reads the profile with
  `GetSettings` and writes it back with `Update`, changing only `autoconnect`.
  `GetSettings` omits secrets, and NetworkManager keeps the stored ones when an
  update carries none: `update_complete()` in `nm-settings-connection.c`.)
  **Forget** deletes the profile, password included (`Settings.Connection.Delete`);
  it asks once (**Confirm**) before doing it.
- **Security**: the details name the security as the user knows it: `Open`,
  `Enhanced Open`, `WEP`, `WPA`, `WPA2`, `WPA/WPA2`, `WPA2/WPA3`, `WPA3`, `WPA2
  Enterprise`, `WPA3 Enterprise` (`classify_wifi_security()`, from the access
  point's `Flags`, `WpaFlags` and `RsnFlags`). A new connection is set up to
  match: `wpa-psk` for WPA/WPA2 (and transition networks), `sae` for WPA3-only
  networks (which reject WPA-PSK), `owe` for Enhanced Open, a WEP key for WEP. A
  network added through the hidden-network form is marked `hidden`, so
  NetworkManager probes for it when it reconnects.
- **Feedback**: the row and the header say `Connecting…` until NetworkManager
  connects. If it starts and then gives up, or never starts within 15 s, they
  say the attempt failed.
- **Placeholder**: an empty list explains why (`Turn on Wi-Fi to see networks.`,
  `NetworkManager is not running.`, `Looking for networks…`, `No networks
  match.`).
- **Footer**: **Hidden Network…** opens a form in place of the footer: name and
  password fields with their labels in one column, then Cancel and Connect.
  **Network Settings…** opens `nm-connection-editor`, and is not shown (here or in
  the context menu) when it is not installed.
- **Keyboard**: Up and Down select a network (Home and End jump to the ends),
  Enter or Space opens the selected one, and on an open one does what its button
  says. Tab reaches the row's controls. Escape closes an open row, then the
  hidden-network form, then the popup. Closing the popup answers a pending
  secret-agent prompt as cancelled.
- The tray tooltip is suppressed while the popup is open, since the popup says
  more.

The popup is a `GUI::WindowType::Popup` (an overlay layer surface), so it is
undecorated, out of the window list and takes keyboard focus for the search and
passphrase fields. (Keyboard focus for `on_demand` layer surfaces is provided by
the compositor; see `focus_layer_surface` in `Services/WindowServer/src/LayerShell.cpp`.)
A layer surface has a fixed size, so the popup is hidden and shown again when
its size changes; it puts the keyboard focus back afterwards.

For the tests, `WIFI_APPLET_LOG_LAYOUT=1` makes the popup log where each row is on
screen, so a test can click a network by name.

## 5. Running on the Taskbar

The applet declares itself with `window->set_window_type(GUI::WindowType::Applet)`
and the title `WiFi`; LibWindowServer maps that to a top-layer layer surface in
the `serenity-applet:WiFi` namespace, and the compositor's `AppletManager` lays it
out in the Taskbar's applet area.

Applet order is right-to-left by descending index, so the compositor appends
**`WiFi` then `Battery`** to `[Applet] Order` (unless the user placed them), which
puts Wi-Fi immediately to the left of the battery applet (`Services/WindowServer/src/Config.cpp`).
The session scripts build it on demand and add it to the compositor's startup
command, next to the battery applet.

## 6. Command line

```
WifiApplet [--name <name>]
```

`--name` sets the applet name (the window title), for `[Applet] Order`; it
defaults to `WiFi`.

## 7. Tests

`scripts/run-wifi-applet-test.sh` is hermetic: it starts a **private D-Bus**
(`dbus-daemon`), serves a **mock NetworkManager** (`Tools/mock-networkmanager.c`)
with two networks (`MockNet`, a secured 82 % network that is connected and has a
saved profile, and the secured `OpenGuest`) and runs the compositor headless. It
asserts:

- the applet reads the mock state (`WifiApplet: 2 networks, connected to MockNet (82%)`);
- it maps as the `serenity-applet:WiFi` layer surface;
- a passphrase typed into the popup reaches `AddAndActivateConnection` intact;
- the applet area places Wi-Fi to the left of Battery.

`scripts/run-wifi-client-test.sh` tests `NetworkManagerClient` on its own,
compiled from the applet's source, against the mock on private buses it starts
and kills itself. It runs in about 30 s and covers:

- the data: networks, strength, security, saved profiles, the sort order;
- every action, and its effect read back: scan, disconnect, radio off and on,
  activating a saved network through the secret agent (passphrase given, and
  cancelled), adding a new one;
- a slow NetworkManager (300 ms per call), a stuck call, and a busy one (a signal
  every 20 ms): the refresh completes, the latest state wins, and the event loop
  never stalls (a 5 ms ticker runs throughout; the longest gap must stay under
  150 ms; the old synchronous client stalled for 2 to 3 s);
- NetworkManager restarting, the bus restarting, the bus coming up late, and the
  client being destroyed with calls in flight;
- the security table (every type, from access point flags);
- turning auto-connect off and on (the mock checks the profile came back whole
  and without secrets) and forgetting a network;
- joining a WPA3-only network with `sae`, and a hidden one marked `hidden`.

The mock's fault injection (`--delay-ms`, `--hang-ap`, `--chatter-ms`) is what
makes these cases deterministic; `--extra-aps` adds one network of each remaining
security type, for looking at the popup. Pass case names (`slow`, `bus-restart`, ...) to
run a subset.

## 8. Known limitations

- **No VPN / mobile-broadband parity.** KDE's applet also manages VPNs and WWAN;
  this applet currently covers Wi-Fi (and wired is out of scope for a Wi-Fi applet).
- **Connectivity is informational.** The `Connectivity`/captive-portal states are
  read but there is no "log in to this network" affordance yet.
- **No 802.1X set-up.** Enterprise networks need an identity, an EAP method and
  certificates; the popup sends the user to Network Settings for those, and
  connects to them once a profile exists.
- **The popup does not close on an outside click** (like the battery applet's): it
  closes on the applet click or Escape.
