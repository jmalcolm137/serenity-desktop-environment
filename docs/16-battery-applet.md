# 16: Battery Applet

**Status:** Implemented · **Date:** 2026-10-11
**Related:** [`05-r3-plan.md`](05-r3-plan.md) §WS4 (applet area) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`14-r4.4-config.md`](14-r4.4-config.md)

`BatteryApplet` is the SDE's battery status applet for the Taskbar's applet area.
It is modeled on KDE Plasma's **Battery and Brightness** applet: a compact tray
icon whose fill tracks the charge level, a charging bolt while plugged in, an
optional percentage label, a tooltip with the state and time remaining, and a
detail popup opened by clicking the icon.

It is a plain SerenityOS LibGUI application (like the portal apps in this repo),
built against the Serenity userland libraries from the `libgui-wayland` build.

## 1. Where it lives

| Path | Role |
|---|---|
| `BatteryApplet/src/Battery.h` / `Battery.cpp` | The power-supply reader and the aggregated summary. |
| `BatteryApplet/src/BatteryWidget.h` / `BatteryWidget.cpp` | The compact tray widget, the tooltip and the detail popup. |
| `BatteryApplet/src/main.cpp` | `serenity_main`, the applet window and the command line. |
| `scripts/build-battery-applet.sh` | Builds it into `build/BatteryApplet/BatteryApplet`. |
| `scripts/run-battery-applet-test.sh` | The headless exit test. |

## 2. Reading the battery

The applet reads the Linux **power-supply class** (`/sys/class/power_supply`)
directly, so it needs no UPower or D-Bus dependency. This is host integration
and therefore lives in the SDE, not in LibGUI/LibCore (see conventions D4).

For every directory whose `type` is `Battery` or `UPS` and that is `present`, the
reader takes:

- the charge percentage: `charge_now`/`charge_full` (or
  `energy_now`/`energy_full`), falling back to `capacity`; a battery that reports
  `Full` is shown as 100% (its wear is a separate health value, not the charge);
- `status`: `Charging`, `Discharging`, `Full` or `Not charging`;
- `model_name`: shown in the popup;
- the time remaining, estimated from `charge_now`/`charge_full`/`current_now`
  (or `energy_now`/`energy_full`/`power_now`), matching what
  `upower`/KDE display: time-to-empty while discharging, time-to-full while
  charging.

Several batteries are summarised for the tray icon (the average percentage; a
charging battery dominates the state) and listed individually in the popup.

## 3. The UI

KDE's battery icon is drawn with LibGfx rather than theme icons, so the applet
needs no external assets:

- a 16×16 battery body and terminal, filled left-to-right by the charge level;
- Breeze-inspired charge colours: red at or below 5 %, orange at or below 20 %,
  green above (matching KDE's `batteryColor` thresholds);
- a lightning bolt overlaid while charging;
- a plug when there is no battery, for an AC-powered desktop (instead of an
  empty battery);
- an optional percentage label (default on; `[Applet]`-style toggle via the
  right-click context menu, or `--icon-only`).

Hovering shows a tooltip in KDE's form:

```
Battery: 66% (2:14 remaining)
Battery: 66% (0:33 until full)
```

Left-clicking opens a frameless popup (`GUI::WindowType::Popup`, an overlay layer
surface) with one row per battery: its name and model, a charge bar, the
percentage and the time remaining. It is positioned above the applet.

## 4. Running on the Taskbar

The applet declares itself with `window->set_window_type(GUI::WindowType::Applet)`
and the title `Battery`. LibWindowServer maps that to a top-layer layer surface in
the `serenity-applet:Battery` namespace, and the compositor's `AppletManager`
lays it out in the Taskbar's applet area (`docs/05-r3-plan.md` §WS4).

The session startup scripts build it on demand and add it to the compositor's
startup command, so it appears with the Taskbar:

```sh
# scripts/start-sde-nested.sh / scripts/start-sde-session.sh
STARTUP="... & $SDE_ROOT/build/BatteryApplet/BatteryApplet"
```

To give it a fixed place among other applets, add `Battery` to `[Applet] Order`
in `WindowServer.ini` (see `docs/14-r4.4-config.md` §2). Unlisted applets still
run; they sort with the other unmatched names.

## 5. Command line

```
BatteryApplet [--name <name>] [--icon-only] [--power-supply-root <path>]
```

- `--name`: the applet name (the window title), for `[Applet] Order`.
  Defaults to `Battery`.
- `--icon-only`: hide the percentage label.
- `--power-supply-root`: read from another directory instead of
  `/sys/class/power_supply` (used by the test).

## 6. Exit test

`scripts/run-battery-applet-test.sh` runs the compositor headless and points the
applet at fixture directories, so it needs no battery hardware and no host
compositor:

- a charging fixture reports `1 batteries, 42%, Charging`;
- an empty directory reports `no battery`;
- in both cases the applet maps as `serenity-applet:Battery` and the
  compositor places it in the applet area.

## 7. Known limitations

- The popup is toggled by clicking the applet (like the Audio applet's popup); it
  does not yet close when clicking elsewhere.
- Charge colour thresholds are the Breeze defaults, not theme colours.
- Only the compact battery/charge/state view is implemented; KDE's brightness
  slider is out of scope.
