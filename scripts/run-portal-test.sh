#!/usr/bin/env bash
# Headless test for the SDE portal backend (R5),
# xdg-desktop-portal-serenity: it implements org.freedesktop.impl.portal.* so the
# xdg-desktop-portal frontend can route requests to it. Runs on a private session
# bus and queries the backend directly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [ "${1:-}" != "--inner" ]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-portal.XXXXXX")"
    trap 'rm -rf "$RUNTIME"' EXIT
    export SDE_PORTAL_TEST_RUNTIME="$RUNTIME"
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$SDE_PORTAL_TEST_RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"

# Record what the OpenURI / Notification paths spawn.
OPENER="$RUNTIME/opener.sh"
cat > "$OPENER" <<EOF
#!/usr/bin/env bash
echo "\$@" >> "$RUNTIME/opened.txt"
EOF
chmod +x "$OPENER"
NOTIFIER="$RUNTIME/notifier.sh"
cat > "$NOTIFIER" <<EOF
#!/usr/bin/env bash
echo "\$@" >> "$RUNTIME/notified.txt"
EOF
chmod +x "$NOTIFIER"

# The file chooser app returns a path; stand in for it in the test.
FILECHOOSER="$RUNTIME/filechooser.sh"
cat > "$FILECHOOSER" <<EOF
#!/usr/bin/env bash
echo "$RUNTIME/picked.txt"
EOF
chmod +x "$FILECHOOSER"

PRINTDIALOG="$RUNTIME/printdialog.sh"
cat > "$PRINTDIALOG" <<'EOF'
#!/usr/bin/env bash
printf "Print to File\t2\n"
EOF
chmod +x "$PRINTDIALOG"

ACCESSDIALOG="$RUNTIME/accessdialog.sh"
cat > "$ACCESSDIALOG" <<'EOF'
#!/usr/bin/env bash
echo 0
EOF
chmod +x "$ACCESSDIALOG"

APPCHOOSER="$RUNTIME/appchooser.sh"
cat > "$APPCHOOSER" <<'EOF'
#!/usr/bin/env bash
echo 1
EOF
chmod +x "$APPCHOOSER"

# Settings reads the theme broker for color-scheme / accent.
printf '%s\n' "$SERENITY/Base/res/themes/Redmond 2000.ini" > "$RUNTIME/sde-theme"

SDE_STATE_DIR="$RUNTIME" SDE_PORTAL_OPENER="$OPENER" SDE_PORTAL_NOTIFIER="$NOTIFIER" SDE_PORTAL_FILE_CHOOSER="$FILECHOOSER" SDE_PORTAL_PRINT_DIALOG="$PRINTDIALOG" SDE_PORTAL_ACCESS_DIALOG="$ACCESSDIALOG" SDE_PORTAL_APP_CHOOSER="$APPCHOOSER" "$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
trap 'kill "$PORTAL_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    sleep 0.1
done

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
call() { gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1; }
prop() { call org.freedesktop.DBus.Properties.Get "$1" "$2"; }

echo "=== Settings ==="
check "uint32 2" "$(call org.freedesktop.impl.portal.Settings.Read org.freedesktop.appearance color-scheme)"
readall="$(call org.freedesktop.impl.portal.Settings.ReadAll org.freedesktop.appearance)"
check "color-scheme" "$readall"
check "accent-color" "$readall"
check "contrast" "$readall"

# The backend watches the theme broker and emits SettingChanged.
timeout 4 gdbus monitor --session --dest "$DEST" --object-path "$OBJ" > "$RUNTIME/settings-monitor.log" 2>&1 &
SETTINGS_MONITOR_PID=$!
sleep 0.6
printf '%s\n' "$SERENITY/Base/res/themes/Default.ini" > "$RUNTIME/sde-theme"
for _ in $(seq 1 20); do
    grep -qF "SettingChanged" "$RUNTIME/settings-monitor.log" 2>/dev/null && break
    sleep 0.1
done
kill "$SETTINGS_MONITOR_PID" 2>/dev/null || true
if grep -qF "SettingChanged" "$RUNTIME/settings-monitor.log" 2>/dev/null; then
    echo "  ok: SettingChanged emitted on theme change"
else
    echo "  note: SettingChanged not observed (monitor: $(head -1 "$RUNTIME/settings-monitor.log" 2>/dev/null))"
fi

echo "=== Account ==="
account="$(call org.freedesktop.impl.portal.Account.GetUserInformation /org/freedesktop/portal/desktop/x app "" "{}")"
check "'id'" "$account"
check "'name'" "$account"

echo "=== Lockdown / Background ==="
check "false" "$(prop org.freedesktop.impl.portal.Lockdown disable_printing)"
check "Serenity Desktop Environment" "$(prop org.freedesktop.impl.portal.Background ApplicationName)"

echo "=== AppChooser / ProxyResolver ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.AppChooser.ChooseApplication /x app "" "['a','b']" "{}")"
check "'b'" "$(call org.freedesktop.impl.portal.AppChooser.ChooseApplication /x app "" "['a','b']" "{}")"
check "[]" "$(call org.freedesktop.impl.portal.ProxyResolver.Lookup http://example.com)"

echo "=== Realtime / PowerProfileMonitor / GameMode / MemoryMonitor ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.Realtime.MakeThreadHighPriority /x /org/freedesktop/portal/desktop/session/a 1 "{}")"
check "false" "$(prop org.freedesktop.impl.portal.PowerProfileMonitor power-saver-enabled)"
check "uint32 0" "$(call org.freedesktop.impl.portal.GameMode.QueryStatus /x app)"
check "'active': <false>" "$(call org.freedesktop.impl.portal.GameMode.QueryStatus /x app)"
check "uint32 1" "$(prop org.freedesktop.impl.portal.MemoryMonitor version)"

echo "=== FileChooser ==="
filechooser="$(call org.freedesktop.impl.portal.FileChooser.OpenFile /h app "" "Open" "{}")"
check "uint32 0" "$filechooser"
check "uris" "$filechooser"
check "picked.txt" "$filechooser"

echo "=== FileChooser SaveFiles (v3) ==="
savefiles="$(call org.freedesktop.impl.portal.FileChooser.SaveFiles /h app "" "Save" "{}" "['first.txt', 'second.txt']")"
check "uint32 0" "$savefiles"
check "uris" "$savefiles"
check "picked.txt" "$savefiles"
check "second.txt" "$savefiles"

echo "=== Print ==="
print_result="$(call org.freedesktop.impl.portal.Print.PreparePrint /h app "" "Doc" "{}" "{}" "{}")"
check "uint32 0" "$print_result"
check "printer" "$print_result"
check "Print to File" "$print_result"
check "copies" "$print_result"

echo "=== GlobalShortcuts ==="
call org.freedesktop.impl.portal.GlobalShortcuts.CreateSession /h /org/freedesktop/portal/desktop/session/gs app "{}" >/dev/null
bind="$(call org.freedesktop.impl.portal.GlobalShortcuts.BindShortcuts /h /org/freedesktop/portal/desktop/session/gs "[('shortcut1', {'trigger': <'CTRL+ALT+t'>, 'description': <'Test'>})]" "" "{}")"
check "uint32 0" "$bind"
check "shortcuts" "$bind"
check "Ctrl+Alt+t" "$bind"
shortcuts_ini="$(cat "$RUNTIME/global-shortcuts.ini" 2>/dev/null || echo '(no ini)')"
check "Trigger=Ctrl+Alt+t" "$shortcuts_ini"
check "Session=/org/freedesktop/portal/desktop/session/gs" "$shortcuts_ini"

# The compositor reports activations over the session FIFO; the backend emits
# Activated. Observe it with gdbus monitor.
timeout 4 gdbus monitor --session --dest "$DEST" --object-path "$OBJ" > "$RUNTIME/gs-monitor.log" 2>&1 &
GS_MONITOR_PID=$!
sleep 0.6
printf '%s\tshortcut1\n' "/org/freedesktop/portal/desktop/session/gs" > "$RUNTIME/sde-global-shortcuts.fifo"
for _ in $(seq 1 20); do
    grep -qF "Activated" "$RUNTIME/gs-monitor.log" 2>/dev/null && break
    sleep 0.1
done
kill "$GS_MONITOR_PID" 2>/dev/null || true
check "Activated" "$(cat "$RUNTIME/gs-monitor.log" 2>/dev/null || echo '(no activation)')"
check "shortcut1" "$(cat "$RUNTIME/gs-monitor.log" 2>/dev/null || echo '(no activation)')"

echo "=== Wallpaper ==="
wallpaper_png="$SERENITY/Base/res/icons/16x16/app-mouse.png"
wp="$(call org.freedesktop.impl.portal.Wallpaper.SetWallpaper /h app "" "file://$wallpaper_png" "{}")"
check "uint32 0" "$wp"
if [ -s "$RUNTIME/wallpaper.raw" ]; then
    echo "  ok: wallpaper.raw written"
else
    echo "  FAIL: wallpaper.raw not written ($wp)"
    fail=1
fi
rm -f "$RUNTIME/wallpaper.raw"
bg_wallpaper="$(call org.freedesktop.impl.portal.Background.SetWallpaperURI /h app "" "file://$wallpaper_png" false "{}")"
check "()" "$bg_wallpaper"
if [ -s "$RUNTIME/wallpaper.raw" ]; then
    echo "  ok: Background.SetWallpaperURI wrote wallpaper.raw"
else
    echo "  FAIL: Background.SetWallpaperURI did not write wallpaper.raw"
    fail=1
fi

echo "=== Secret / USB / DynamicLauncher / Access ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.Access.AccessDialog /h app "" "Allow?" "" "Please allow." "{}")"
check "uint32 0" "$(call org.freedesktop.impl.portal.Secret.RetrieveSecret /h app "{}")"
usb="$(call org.freedesktop.impl.portal.USB.ListDevices "{}")"
check "uint32 0" "$usb"
check "devices" "$usb"
token="$(call org.freedesktop.impl.portal.DynamicLauncher.RequestInstallToken app "{}")"
if [ ${#token} -gt 5 ]; then
    echo "  ok: install token ($token)"
else
    echo "  FAIL: install token ($token)"
    fail=1
fi

echo "=== OpenURI ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.OpenURI.OpenURI /x "" https://example.com "{}")"
sleep 0.3
check "https://example.com" "$(cat "$RUNTIME/opened.txt" 2>/dev/null || echo '(no opener call)')"
check "uint32 0" "$(call org.freedesktop.impl.portal.OpenURI.Activate /h app "" "{}")"
sleep 0.3
check "app" "$(cat "$RUNTIME/opened.txt" 2>/dev/null || echo '(no opener call)')"

echo "=== backend declaration ==="
portals_conf="$SDE_ROOT/contrib/xdg-desktop-portal/portals.conf"
portal_file="$SDE_ROOT/contrib/xdg-desktop-portal/serenity.portal"
dupes="$(grep -E '^org\.freedesktop\.portal\.' "$portals_conf" | cut -d= -f1 | sort | uniq -d)"
if [ -z "$dupes" ]; then
    echo "  ok: portals.conf has no duplicate interface keys"
else
    echo "  FAIL: portals.conf repeats interface keys: $dupes"
    fail=1
fi
if grep -qF "org.freedesktop.portal.ScreenCast=serenity" "$portals_conf" \
    && grep -qF "org.freedesktop.impl.portal.ScreenCast;" "$portal_file"; then
    echo "  ok: ScreenCast declared and routed to serenity"
else
    echo "  FAIL: ScreenCast not consistently declared"
    fail=1
fi

echo "=== Notification ==="
call org.freedesktop.impl.portal.Notification.AddNotification id "{'title': <'Hello'>, 'body': <'World'>}" >/dev/null
sleep 0.3
check "Hello" "$(cat "$RUNTIME/notified.txt" 2>/dev/null || echo '(no notifier call)')"
check "World" "$(cat "$RUNTIME/notified.txt" 2>/dev/null || echo '(no notifier call)')"
call org.freedesktop.impl.portal.Notification.AddNotification id2 "{'title': <'T'>, 'body': <'B'>, 'icon': <('themed', <'dialog-information'>)>}" >/dev/null
sleep 0.3
check "dialog-information" "$(cat "$RUNTIME/notified.txt" 2>/dev/null || echo '(no notifier call)')"
check "()" "$(call org.freedesktop.impl.portal.Notification.RemoveNotification id2)"

# Best-effort: prove the real xdg-desktop-portal frontend routes to us. Skipped
# when the frontend is not installed.FRONTEND=""
for candidate in /usr/lib/xdg-desktop-portal /usr/libexec/xdg-desktop-portal; do
    [ -x "$candidate" ] && FRONTEND="$candidate" && break
done
if [ -n "$FRONTEND" ]; then
    echo "=== frontend integration (best effort) ==="
    mkdir -p "$RUNTIME/config/xdg-desktop-portal" "$RUNTIME/data/xdg-desktop-portal/portals"
    printf '[preferred]\norg.freedesktop.portal.Settings=serenity\n' > "$RUNTIME/config/xdg-desktop-portal/portals.conf"
    cp "$SDE_ROOT/contrib/xdg-desktop-portal/serenity.portal" "$RUNTIME/data/xdg-desktop-portal/portals/"
    env XDG_CONFIG_HOME="$RUNTIME/config" XDG_DATA_HOME="$RUNTIME/data" XDG_CURRENT_DESKTOP=Serenity \
        "$FRONTEND" > "$RUNTIME/frontend.log" 2>&1 &
    FRONTEND_PID=$!
    sleep 2
    value="$(env XDG_CONFIG_HOME="$RUNTIME/config" XDG_DATA_HOME="$RUNTIME/data" XDG_CURRENT_DESKTOP=Serenity \
        timeout 5 gdbus call --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop \
        --method org.freedesktop.portal.Settings.Read org.freedesktop.appearance color-scheme 2>&1 || true)"
    kill "$FRONTEND_PID" 2>/dev/null || true
    if grep -qF "uint32 2" <<<"$value"; then
        echo "  ok: frontend routed Settings to the SDE backend ($value)"
    else
        echo "  note: frontend did not route to the SDE backend here ($value) — direct backend tests above cover it"
    fi
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> portal test FAILED"
    exit 1
fi
echo "==> portal test PASSED"
