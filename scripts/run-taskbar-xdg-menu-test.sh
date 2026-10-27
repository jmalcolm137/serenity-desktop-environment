#!/usr/bin/env bash
# Taskbar XDG menu test.
#
# Exercises the freedesktop Menu specification path:
#  - a user applications.menu that merges its <MergeFile type="parent"> system
#    menu, so <Include>/<Exclude>, <OnlyUnallocated>, duplicate-menu
#    consolidation and <Layout>/<DefaultLayout> are all exercised;
#  - .directory entries provide the labels (and localized labels are read);
#  - the SDE's shipped Base/xdg/menus/applications.menu is the fallback when the
#    host has no ${XDG_MENU_PREFIX}applications.menu;
#  - with no menu file anywhere, the old category menu is the last resort;
#  - XDGMenu=false still selects the unmodified Serenity .af menu.
#
# Runs entirely headless in a private runtime/config/data tree, so it never
# touches a live session or the developer's menus.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

if [[ "${SDE_TEST_NO_BUILD:-0}" != "1" ]]; then
    "$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
    "$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null
fi

[[ -x "$BUILD/bin/Taskbar" ]] || { echo "SKIP: Taskbar not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-taskbar-xdg.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME/run"
export SDE_STATE_DIR="$RUNTIME/state"
export SDE_SESSION_DIR="$RUNTIME/session"
export SDE_WM_PORTAL_PATH="$RUNTIME/wm"
mkdir -p "$XDG_RUNTIME_DIR" "$SDE_STATE_DIR" "$SDE_SESSION_DIR/portal" "$SDE_SESSION_DIR/sbin"
mkdir -p "$RUNTIME/config/menus" "$RUNTIME/config-dirs/menus" \
    "$RUNTIME/empty-config" "$RUNTIME/empty-config-dirs" \
    "$RUNTIME/data-home/applications" "$RUNTIME/data-home/desktop-directories"

XDG_DATA="$RUNTIME/xdg-data"
"$SDE_ROOT/scripts/generate-desktop-entries.sh" --source "$SERENITY/Base/res/apps" --output "$XDG_DATA" >/dev/null
entry_count="$(find "$XDG_DATA/applications" -name '*.desktop' | wc -l)"
echo "  note: generated $entry_count .desktop entries"

# --- the system menu (merged by the user menu) -----------------------------
cat > "$RUNTIME/config-dirs/menus/applications.menu" <<'XML'
<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN"
 "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
    <Name>Applications</Name>
    <DefaultAppDirs/>
    <DefaultDirectoryDirs/>
    <Menu>
        <Name>Games</Name>
        <Directory>test-games.directory</Directory>
        <Include><Category>Game</Category></Include>
    </Menu>
    <Menu>
        <Name>Hidden</Name>
        <Directory>test-hidden.directory</Directory>
        <Include><All/></Include>
        <Deleted/>
    </Menu>
    <Menu>
        <Name>Temporary</Name>
        <Include><Filename>2048.desktop</Filename></Include>
    </Menu>
    <Move>
        <Old>Temporary</Old>
        <New>Renamed</New>
    </Move>
    <Menu>
        <Name>Other</Name>
        <Directory>test-other.directory</Directory>
        <OnlyUnallocated/>
        <Include><All/></Include>
    </Menu>
</Menu>
XML

# --- the user menu: parent merge, layout, duplicate Games, exclusions -------
cat > "$RUNTIME/config/menus/applications.menu" <<'XML'
<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN"
 "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
    <Name>Applications</Name>
    <MergeFile type="parent">ignored-name.menu</MergeFile>

    <!-- A root-level entry, so <Merge type="all"> has files and menus to mix. -->
    <Include><Filename>Calculator.desktop</Filename></Include>

    <Menu>
        <Name>Tools</Name>
        <Directory>test-tools.directory</Directory>
        <Include><Category>Utility</Category></Include>
        <Exclude><Filename>2048.desktop</Filename></Exclude>
        <Layout>
            <Filename>Calculator.desktop</Filename>
            <Separator/>
            <Merge type="files"/>
        </Layout>
    </Menu>

    <Menu>
        <Name>Merged</Name>
        <Directory>test-merged.directory</Directory>
        <Include><Filename>2048.desktop</Filename></Include>
    </Menu>

    <!-- Same name as the merged system menu: the last Directory wins, and the
         entries of both menus are consolidated. -->
    <Menu>
        <Name>Games</Name>
        <Directory>test-games-merged.directory</Directory>
        <Include><Filename>Calculator.desktop</Filename></Include>
    </Menu>

    <Layout>
        <Menuname>Merged</Menuname>
        <Merge type="all"/>
    </Layout>
</Menu>
XML

# --- directory entries -----------------------------------------------------
write_directory() {
    local file="$1" name="$2"
    printf '[Desktop Entry]\nType=Directory\nName=%s\nIcon=utilities\n' "$name" \
        > "$RUNTIME/data-home/desktop-directories/$file"
}
write_directory test-tools.directory 'Tools & Accessories'
write_directory test-games.directory 'Games'
write_directory test-games-merged.directory 'All Games'
write_directory test-merged.directory 'Merged'
write_directory test-other.directory 'Other'
write_directory test-hidden.directory 'Hidden'
printf 'Name[en_CA]=Autres\n' >> "$RUNTIME/data-home/desktop-directories/test-other.directory"

# --- extra desktop entries: filtering, localization, Link, TryExec ---------
cat > "$RUNTIME/data-home/applications/Orphan.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Orphan
Name[en_CA]=Orphan Localised
Exec=Calculator
Categories=X-Test;
Icon=app-calculator
EOF
cat > "$RUNTIME/data-home/applications/SdeOnly.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=SDE Only
Exec=Calculator
Categories=Utility;
OnlyShowIn=SDE;
EOF
cat > "$RUNTIME/data-home/applications/KdeOnly.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=KDE Only
Exec=Calculator
Categories=Utility;
OnlyShowIn=KDE;
EOF
cat > "$RUNTIME/data-home/applications/NoDisplay.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Hidden Entry
Exec=Calculator
Categories=Utility;
NoDisplay=true
EOF
cat > "$RUNTIME/data-home/applications/Broken.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Broken Entry
Exec=Calculator
TryExec=/nonexistent/broken
Categories=Utility;
EOF
cat > "$RUNTIME/data-home/applications/Link.desktop" <<'EOF'
[Desktop Entry]
Type=Link
Name=Serenity Site
URL=https://serenityos.org/
Categories=X-Test;
Icon=app-browser
EOF

COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

run_taskbar() {
    local label="$1" menu="$2" sock="$3"
    shift 3
    cat > "$SDE_STATE_DIR/WindowServer.ini" <<EOF
[Taskbar]
XDGMenu=$menu
EOF
    setsid env -u WAYLAND_DISPLAY -u DISPLAY -u LC_ALL -u LC_MESSAGES \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_XWAYLAND=0 \
        LANG=en_CA.UTF-8 LANGUAGE= \
        XDG_CURRENT_DESKTOP=SDE \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        SDE_SESSION_BIN_DIR="$SDE_SESSION_DIR/sbin" \
        XDG_DATA_DIRS="$XDG_DATA" \
        "$@" \
        "$COMP" -n "$sock" -s "$BUILD/bin/Taskbar" > "$RUNTIME/comp-$label.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 300); do
        grep -q "layer surface namespace='serenity-taskbar'" "$RUNTIME/comp-$label.log" 2>/dev/null && break
        sleep 0.1
    done
    sleep 1
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    sleep 0.3
    kill -KILL -- "-$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
}

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
check_absent() {
    if grep -qF "$1" "$2"; then
        echo "  UNEXPECTED ($2): $1"
        fail=1
    else
        echo "  ok: absent: $1"
    fi
}
check_text() {
    if [[ "$2" == *"$1"* ]]; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1 (in '$2')"
        fail=1
    fi
}
check_text_absent() {
    if [[ "$2" == *"$1"* ]]; then
        echo "  UNEXPECTED: $1 (in '$2')"
        fail=1
    else
        echo "  ok: absent: $1"
    fi
}
menu_line() {
    grep -F "Taskbar: XDG menu '$1':" "$2" | head -1
}

echo "=== full menu: user file merging the system file ==="
run_taskbar full true "sde-taskbar-full-$$" \
    XDG_CONFIG_HOME="$RUNTIME/config" \
    XDG_CONFIG_DIRS="$RUNTIME/config-dirs" \
    XDG_DATA_HOME="$RUNTIME/data-home" \
    SDE_MENU_DIR="$RUNTIME/no-such-menu-dir"
FULL="$RUNTIME/comp-full.log"
check "Taskbar: using XDG menu file '$RUNTIME/config/menus/applications.menu'" "$FULL"
check "Taskbar: XDG menu merged '$RUNTIME/config-dirs/menus/applications.menu'" "$FULL"
check_text "Taskbar: XDG menu 'All Games': 2048" "$(menu_line 'All Games' "$FULL")"
check_text "Calculator" "$(menu_line 'All Games' "$FULL")"
check_text "Taskbar: XDG menu 'Merged': 2048" "$(menu_line 'Merged' "$FULL")"
check_text "Calculator, ---, 3D File Viewer" "$(menu_line 'Tools & Accessories' "$FULL")"
check_text "SDE Only" "$(menu_line 'Tools & Accessories' "$FULL")"
check_text_absent "2048" "$(menu_line 'Tools & Accessories' "$FULL")"
check_text "Taskbar: XDG menu 'Autres':" "$(menu_line 'Autres' "$FULL")"
check_text "Orphan Localised" "$(menu_line 'Autres' "$FULL")"
check_text "Serenity Site" "$(menu_line 'Autres' "$FULL")"
check_text_absent "Calculator" "$(menu_line 'Autres' "$FULL")"
check_text "Taskbar: XDG menu 'Renamed': 2048" "$(menu_line 'Renamed' "$FULL")"
check_absent "XDG menu 'Temporary'" "$FULL"
check_text "[Merged], [All Games], [Autres], Calculator, [Renamed], [Tools & Accessories]" "$(menu_line 'Applications' "$FULL")"
check_absent "KDE Only" "$FULL"
check_absent "Hidden Entry" "$FULL"
check_absent "Broken Entry" "$FULL"
check_absent "XDG menu 'Hidden'" "$FULL"

echo "=== shipped default menu (no host applications.menu) ==="
run_taskbar shipped true "sde-taskbar-shipped-$$" \
    XDG_CONFIG_HOME="$RUNTIME/empty-config" \
    XDG_CONFIG_DIRS="$RUNTIME/empty-config-dirs" \
    XDG_DATA_HOME="$RUNTIME/data-home" \
    SDE_MENU_DIR="$SDE_ROOT/Base/xdg/menus"
SHIPPED="$RUNTIME/comp-shipped.log"
check "Taskbar: using XDG menu file '$SDE_ROOT/Base/xdg/menus/applications.menu'" "$SHIPPED"
check_text "Taskbar: XDG menu 'Games': 2048" "$(menu_line 'Games' "$SHIPPED")"
check_text "Taskbar: XDG menu 'Utilities': 3D File Viewer" "$(menu_line 'Utilities' "$SHIPPED")"
check_text "[Games]" "$(menu_line 'Applications' "$SHIPPED")"

echo "=== fallback: no menu file anywhere ==="
run_taskbar fallback true "sde-taskbar-fallback-$$" \
    XDG_CONFIG_HOME="$RUNTIME/empty-config" \
    XDG_CONFIG_DIRS="$RUNTIME/empty-config-dirs" \
    XDG_DATA_HOME="$RUNTIME/data-home" \
    SDE_MENU_DIR="$RUNTIME/no-such-menu-dir"
FALLBACK="$RUNTIME/comp-fallback.log"
check "Taskbar: no XDG menu file found; using the category fallback" "$FALLBACK"
check "Taskbar: XDG fallback menu '&Games'" "$FALLBACK"
check "Taskbar: XDG fallback menu '&Utilities'" "$FALLBACK"

echo "=== Serenity menu (default) ==="
run_taskbar ser false "sde-taskbar-ser-$$" \
    XDG_CONFIG_HOME="$RUNTIME/empty-config" \
    XDG_CONFIG_DIRS="$RUNTIME/empty-config-dirs" \
    XDG_DATA_HOME="$RUNTIME/data-home" \
    SDE_MENU_DIR="$SDE_ROOT/Base/xdg/menus"
SERENITY_LOG="$RUNTIME/comp-ser.log"
check "Taskbar: using Serenity menu" "$SERENITY_LOG"
check "Taskbar: system menu discovered" "$SERENITY_LOG"
check_absent "Taskbar: using XDG menu" "$SERENITY_LOG"

if [[ "$fail" -ne 0 ]]; then
    echo "==> taskbar XDG menu test FAILED"
    for log in full shipped fallback ser; do
        echo "--- $log log ---"
        grep -E "Taskbar: (using|XDG|no XDG)" "$RUNTIME/comp-$log.log" | head -40 || true
    done
    exit 1
fi
echo "==> taskbar XDG menu test PASSED"
