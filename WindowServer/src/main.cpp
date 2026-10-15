/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Appearance.h"
#include "AppletManager.h"
#include "DragAndDrop.h"
#include "Focus.h"
#include "Frame.h"
#include "Idle.h"
#include "InputCapture.h"
#include "InputDevices.h"
#include "InputMethod.h"
#include "Interop.h"
#include "Keyboard.h"
#include "LayerShell.h"
#include "Output.h"
#include "OutputManagement.h"
#include "Performance.h"
#include "Pointer.h"
#include "Resources.h"
#include "SerenityWindow.h"
#include "SessionLock.h"
#include "SessionManager.h"
#include "Tablet.h"
#include "VirtualInput.h"
#include "Wallpaper.h"
#include "WindowManager.h"
#include "WindowMenu.h"
#include "WindowSwitcher.h"
#include "XWayland.h"
#include "XWaylandFrame.h"
#include "XdgShell.h"

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Notifier.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>

#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Wlroots.h"

static ErrorOr<void> initialize(Server& server)
{
    server.display = wl_display_create();
    if (!server.display)
        return Error::from_string_literal("failed to create wl_display");

    // R4.3/R6: seat identity. The session exports XDG_SEAT/XDG_VTNR (from the
    // display manager or logind); log them so multi-seat sessions are identifiable.
    if (auto const* seat = getenv("XDG_SEAT"); seat && *seat) {
        if (auto const* vt = getenv("XDG_VTNR"); vt && *vt)
            dbgln("WindowServer: seat '{}' vt{}", seat, vt);
        else
            dbgln("WindowServer: seat '{}'", seat);
    }

    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), nullptr);
    if (!server.backend) {
        // R4.3: the DRM/libinput backend needs a seat (libseat). Give actionable
        // guidance rather than a bare failure so a TTY login is recoverable.
        warnln("WindowServer: could not create a backend (DRM/libinput needs a seat).");
        warnln("  * install and start seatd (recommended), or run under systemd-logind;");
        warnln("  * for direct TTY access (builtin), join the 'video', 'render' and 'input' groups and re-login;");
        warnln("  * or use a nested/headless session (scripts/start-sde-nested.sh).");
        warnln("  * SDE_SEAT_BACKEND/LIBSEAT_BACKEND/SEATD_SOCK select the seat backend.");
        return Error::from_string_literal("failed to create wlr_backend");
    }

    // R6/N5 renderer selection: [Output] Renderer in WindowServer.ini, with the
    // SDE_RENDERER environment variable overriding it (the config-test pattern).
    // It maps onto wlroots' `WLR_RENDERER`.
    ByteString requested_renderer_storage;
    if (auto const* renderer = getenv("SDE_RENDERER"); renderer && *renderer)
        requested_renderer_storage = renderer;
    else
        requested_renderer_storage = Config::output().renderer;
    char const* requested_renderer = requested_renderer_storage.is_empty() ? "auto" : requested_renderer_storage.characters();
    // An explicit WLR_RENDERER in the environment is a hard request (wlroots-level,
    // used to exercise a specific path); the "auto" rule below must not override it.
    bool const wlr_renderer_forced = getenv("WLR_RENDERER") != nullptr;
    if (!requested_renderer_storage.is_empty())
        setenv("WLR_RENDERER", requested_renderer, true);

    // On "auto" with no DRM device (the headless backend, a VM without one) any GL
    // is a software implementation — llvmpipe — which costs ~85 MB of RSS (Mesa's
    // gallium/LLVM footprint) and is no faster than the pixman renderer for the
    // damage patterns we render. Take pixman directly, so the GL stack is never
    // even touched; SDE_RENDERER=gles2 still forces the GL path.
    bool used_software_auto = false;
    if (strcmp(requested_renderer, "auto") == 0 && !wlr_renderer_forced && wlr_backend_get_drm_fd(server.backend) < 0) {
        setenv("WLR_RENDERER", "pixman", true);
        used_software_auto = true;
    }

    bool used_software_fallback = false;
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        // The GPU path failed (no DRM render node, EGL bring-up failure,
        // unsupported driver, ...). Rather than ending the session, retry with
        // wlroots' software (pixman) renderer, which needs no device. (LibGPU/
        // LibSoftGPU are the *client* GL software path; they cannot back a
        // wlr_renderer.)
        wlr_log(WLR_ERROR, "failed to create a GPU renderer; falling back to the software (pixman) renderer");
        setenv("WLR_RENDERER", "pixman", true);
        server.renderer = wlr_renderer_autocreate(server.backend);
        used_software_fallback = true;
    }
    if (!server.renderer)
        return Error::from_string_literal("failed to create wlr_renderer");
    if (used_software_fallback)
        dbgln("WindowServer: renderer 'pixman' (software fallback from '{}')", requested_renderer);
    else if (used_software_auto)
        dbgln("WindowServer: renderer 'pixman' (auto: no DRM device, so any GL would be software)");
    else
        dbgln("WindowServer: renderer '{}'", requested_renderer);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator)
        return Error::from_string_literal("failed to create wlr_allocator");

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // Layer trees, created bottom-to-top: background, bottom, windows, top,
    // overlay. Layer surfaces go into the tree for their layer and ordinary
    // windows into windows_tree, so stacking is correct.
    server.background_tree = wlr_scene_tree_create(&server.scene->tree);
    server.bottom_tree = wlr_scene_tree_create(&server.scene->tree);
    server.windows_tree = wlr_scene_tree_create(&server.scene->tree);
    server.top_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
    // Drag icons render above every layer (R5 explicit drag-and-drop).
    server.drag.icon_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_manager = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(server.cursor_manager, 1);
    wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");

    server.seat = wlr_seat_create(server.display, "seat0");

    // Each subsystem creates its globals and wires its listeners.
    initialize_outputs(server);
    initialize_xdg_shell(server);
    initialize_layer_shell(server);
    initialize_serenity_window(server);
    // Test harness: virtual touch/tablet injection (see serenity-virtual-input).
    initialize_virtual_input(server);
    SessionManager::initialize(server);
    // R5: InputCapture portal support (compositor-hosted EIS; docs/17).
    InputCapture::initialize(server);
    initialize_interop(server);
    initialize_output_management(server);
    initialize_idle(server);
    initialize_session_lock(server);
    initialize_input_devices(server);
    initialize_pointer(server);
    initialize_tablet(server);
    initialize_drag_and_drop(server);
    initialize_xwayland(server);

    // Screen capture. Expose the standard ext-image-copy-capture-v1 (the primary,
    // portable capture surface) and the wlr-screencopy-unstable-v1 compat so
    // standard tools like `grim` work unmodified. See docs/03 D7.
    (void)wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
    (void)wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
    (void)wlr_screencopy_manager_v1_create(server.display);

    // R5: the native-app compatibility surface. Most of these protocols are fully
    // implemented by wlroots, so the compositor only advertises them; the ones
    // that need a little glue (relative pointer, keyboard-shortcuts-inhibit,
    // gestures, tablets, pointer constraints) are set up by their subsystems above.
    wlr_primary_selection_v1_device_manager_create(server.display);
    // data-control lets clipboard managers (e.g. the SDE ClipboardHistory applet)
    // observe and set the selection and primary selection. wlroots implements the
    // protocol; both the wlr and the ext variants are advertised.
    wlr_data_control_manager_v1_create(server.display);
    wlr_ext_data_control_manager_v1_create(server.display, 1);
    wlr_viewporter_create(server.display);
    wlr_content_type_manager_v1_create(server.display, 1);
    wlr_tearing_control_manager_v1_create(server.display, 1);
    // R6: more standard protocols wlroots implements; advertising them is all
    // that is needed (solid-colour buffers, per-surface opacity, modal dialogs).
    wlr_single_pixel_buffer_manager_v1_create(server.display);
    wlr_alpha_modifier_v1_create(server.display);
    wlr_xdg_wm_dialog_v1_create(server.display, 1);
    // R6 presentation-time: frame feedback (presented events). Fed from
    // output_frame() via wlr_presentation_surface_textured_on_output().
    wlr_presentation_create(server.display, server.backend, 2);
    // R6 HiDPI: fractional output scale. wlr_scene notifies each mapped surface of
    // the preferred (possibly fractional) scale, so native clients render at it.
    wlr_fractional_scale_manager_v1_create(server.display, 1);
    auto* foreign_registry = wlr_xdg_foreign_registry_create(server.display);
    wlr_xdg_foreign_v1_create(server.display, foreign_registry);
    wlr_xdg_foreign_v2_create(server.display, foreign_registry);

    // R5 IME: text-input-v3 + input-method-v2 (docs/22-input-method.md).
    InputMethod::initialize(server);

    return { };
}

static ByteString add_socket(Server& server, ByteString const& requested)
{
    if (!requested.is_empty()) {
        if (wl_display_add_socket(server.display, requested.characters()) == 0)
            return requested;
        warnln("WindowServer: could not add socket '{}'", requested);
    }
    auto const* socket = wl_display_add_socket_auto(server.display);
    return socket ? ByteString(socket) : ByteString { };
}

static void spawn_startup_command(StringView command)
{
    if (command.is_empty())
        return;

    // Run each "&"-separated command as the *foreground* job of its own shell.
    //
    // A single `sh -c "A & B"` sets SIGINT and SIGQUIT to SIG_IGN in A and B: POSIX
    // requires a non-interactive shell to ignore those for background jobs, and the
    // ignore is inherited by everything the application goes on to run. A shell
    // started by our Terminal had SIGINT ignored, which is why Ctrl-C did nothing
    // in it, or in any program started from it, whatever the terminal did. Spawning
    // each command as the only job of its own shell leaves the dispositions alone.
    size_t start = 0;
    while (start <= command.length()) {
        size_t end = command.length();
        for (size_t i = start; i + 3 <= command.length(); ++i) {
            if (command.substring_view(i, 3) == " & "sv) {
                end = i;
                break;
            }
        }
        auto piece = command.substring_view(start, end - start).trim_whitespace();
        if (!piece.is_empty()) {
            auto piece_string = piece.to_byte_string();
            if (fork() == 0) {
                // The compositor may itself have been started as a background job
                // (the session supervisor does that), so give the shell that runs
                // the command default dispositions: a foreground command inherits
                // them, and otherwise the ignore would reach the application after
                // all.
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                execl("/bin/sh", "/bin/sh", "-c", piece_string.characters(), static_cast<char*>(nullptr));
                _exit(127);
            }
        }
        if (end >= command.length())
            break;
        start = end + 3;
    }
}

static ErrorOr<void> initialize_gfx()
{
    auto const* res_root = getenv("SERENITY_RES_ROOT");
    if (!res_root || !*res_root)
        return Error::from_string_literal("SERENITY_RES_ROOT is not set; cannot load the Serenity theme");

    ByteString resource_root { res_root };
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(TRY(String::from_byte_string(resource_root))));

    Gfx::FontDatabase::set_default_font_query("Katica 10 400 0"sv);
    Gfx::FontDatabase::set_window_title_font_query("Katica 10 700 0"sv);
    Gfx::FontDatabase::the().load_all_fonts_from_uri("resource://fonts"sv);

    auto theme = TRY(Gfx::load_system_theme(ByteString::formatted("{}/themes/Default.ini", resource_root)));
    Gfx::set_system_theme(theme);
    g_palette = new Gfx::Palette(Gfx::PaletteImpl::create_with_anonymous_buffer(theme));
    g_resource_root = resource_root;
    load_overlay_shadow();

    g_frame_icons.close = load_icon(resource_root, "window-close"sv, "window-close"sv);
    g_frame_icons.minimize = load_icon(resource_root, "window-minimize"sv, "downward-triangle"sv);
    g_frame_icons.maximize = load_icon(resource_root, "window-maximize"sv, "upward-triangle"sv);
    g_frame_icons.restore = load_icon(resource_root, "window-restore"sv, "upward-triangle"sv);
    g_frame_icons.move = load_icon(resource_root, "move"sv, ""sv);
    g_frame_icons.pin = load_icon(resource_root, "window-pin"sv, ""sv);
    g_default_window_icon = load_icon(resource_root, "window"sv, "window"sv);
    g_x11_default_icon = make_x11_default_icon(g_palette ? g_palette->active_window_title() : Gfx::Color::White);

    // Wallpaper/background: load the persisted state, seeding from
    // SDE_WALLPAPER / SDE_WALLPAPER_MODE on the first run.
    initialize_wallpaper(resource_root);

    // The compositor's own configuration (WindowServer.ini) lives beside the
    // wallpaper/theme state; load it now and reload on change.
    Config::load();
    // R6/N8: apply [Log] Level unless SDE_LOG_LEVEL already overrode it.
    if (!getenv("SDE_LOG_LEVEL") && !Config::log().level.is_empty()) {
        if (auto level = log_level_from_string(Config::log().level.view()); level.has_value())
            apply_log_level(*level, Config::log().level.view());
    }
    // R4.5: the persisted output layout (screenlayout.ini) is applied as outputs
    // are created (server_new_output).
    Config::load_outputs();
    // R4.6: input device settings (input.ini) are applied as devices are created
    // (server_new_input).
    Config::load_input();
    // R5: global shortcuts requested by apps (global-shortcuts.ini).
    Config::load_global_shortcuts();

    // R4.7: a persisted font choice ([Fonts]) overrides the Katica default.
    if (!Config::fonts().default_query.is_empty()) {
        Gfx::FontDatabase::set_default_font_query(Config::fonts().default_query);
        if (!Config::fonts().fixed_width_query.is_empty())
            Gfx::FontDatabase::set_fixed_width_font_query(Config::fonts().fixed_width_query);
        if (!Config::fonts().window_title_query.is_empty())
            Gfx::FontDatabase::set_window_title_font_query(Config::fonts().window_title_query);
        dbgln("WindowServer: applied persisted system fonts (default '{}')", Config::fonts().default_query);
    }

    dbgln("WindowServer: loaded the Serenity theme from {}", resource_root);
    return { };
}

// R6/N8 crash-dump policy. A fatal signal appends a short report (including a
// backtrace) to $SDE_CRASH_LOG (default: the state dir's WindowServer-crash.log)
// and then re-raises, so the supervisor restarts the session and we still have a
// diagnostic. The handler only uses async-signal-safe calls (write/backtrace_
// symbols_fd) and never swallows the crash.
static ByteString s_crash_log_path;

static int s_crash_log_fd { -1 };

static void crash_write(char const* data, size_t length)
{
    while (length > 0) {
        auto written = ::write(s_crash_log_fd, data, length);
        if (written <= 0)
            return;
        data += written;
        length -= static_cast<size_t>(written);
    }
}

static void fatal_signal_handler(int signal_number)
{
    char const* name = "signal";
    switch (signal_number) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGBUS: name = "SIGBUS"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGFPE: name = "SIGFPE"; break;
    default: break;
    }
    // open(2) is async-signal-safe, so the log file is created lazily (a normal
    // exit never leaves an empty crash log behind).
    s_crash_log_fd = ::open(s_crash_log_path.characters(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s_crash_log_fd >= 0) {
        char const* header = "\nWindowServer: crashed with ";
        crash_write(header, strlen(header));
        crash_write(name, strlen(name));
        char const* suffix = " (backtrace follows)\n";
        crash_write(suffix, strlen(suffix));
        void* frames[64];
        int frame_count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, frame_count, s_crash_log_fd);
        ::close(s_crash_log_fd);
        s_crash_log_fd = -1;
    }
    // Restore the default action and re-raise, so the process still dies with the
    // signal (the supervisor sees a non-zero exit and restarts).
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void install_crash_report_handler()
{
    if (auto const* configured = getenv("SDE_CRASH_LOG"); configured && *configured)
        s_crash_log_path = configured;
    else
        s_crash_log_path = ByteString::formatted("{}/WindowServer-crash.log", Config::state_directory());
    if (auto directory = LexicalPath(s_crash_log_path).dirname().to_byte_string(); !directory.is_empty())
        (void)Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes);
    struct sigaction action { };
    action.sa_handler = fatal_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    for (int signal_number : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE })
        sigaction(signal_number, &action, nullptr);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    wlr_log_init(WLR_ERROR, nullptr);
    install_crash_report_handler();
    // R6/N8: SDE_LOG_LEVEL overrides [Log] Level (which is applied once the
    // config loads).
    if (auto const* env_level = getenv("SDE_LOG_LEVEL"); env_level && *env_level) {
        StringView env_level_view { env_level, strlen(env_level) };
        if (auto level = log_level_from_string(env_level_view); level.has_value())
            apply_log_level(*level, env_level_view);
    }

    StringView startup_command;
    StringView socket_name;
    bool check_config = false;
    bool print_config = false;
    Core::ArgsParser args_parser;
    args_parser.add_option(startup_command, "Command to run once the compositor is up", nullptr, 's', "command");
    args_parser.add_option(socket_name, "Wayland socket name", nullptr, 'n', "name");
    args_parser.add_option(check_config, "Validate the compositor config and exit", "check-config");
    args_parser.add_option(print_config, "Print the effective compositor config and exit", "print-config");
    args_parser.parse(arguments);

    if (check_config || print_config) {
        Config::load();
        if (print_config) {
            outln("[Workspaces]");
            outln("Rows={}", Config::workspaces().rows);
            outln("Columns={}", Config::workspaces().columns);
            outln("[Tiling]");
            outln("EdgeThreshold={}", Config::tiling().edge_threshold);
            outln("DragToEdge={}", Config::tiling().drag_to_edge);
            outln("[Focus]");
            outln("FollowsMouse={}", Config::focus().follows_mouse);
            outln("[Effects]");
            outln("WindowGeometry={}", Config::effects().window_geometry);
            outln("[Output]");
            outln("DefaultScale={}", Config::output().default_scale);
            outln("Renderer={}", Config::output().renderer.is_empty() ? "auto"sv : Config::output().renderer.view());
            outln("[Input]");
            outln("AccelMin={}", Config::input_ranges().accel_min);
            outln("AccelMax={}", Config::input_ranges().accel_max);
            outln("ScrollStepMin={}", Config::input_ranges().scroll_step_min);
            outln("DoubleClickMin={}", Config::input_ranges().double_click_min);
            outln("DoubleClickMax={}", Config::input_ranges().double_click_max);
            outln("[Fonts]");
            outln("Default={}", Config::fonts().default_query);
            outln("FixedWidth={}", Config::fonts().fixed_width_query);
            outln("WindowTitle={}", Config::fonts().window_title_query);
            outln("[Theme]");
            outln("Name={}", Config::theme().name);
            outln("Path={}", Config::theme().path);
            outln("[Applet]");
            StringBuilder order;
            for (size_t i = 0; i < Config::applet_order().size(); ++i) {
                if (i > 0)
                    order.append(',');
                order.append(Config::applet_order()[i]);
            }
            outln("Order={}", order.to_byte_string());
            outln("[Keybindings]");
            for (size_t i = 0; i < Config::action_names().size(); ++i)
                outln("{}={}", Config::action_names()[i], Config::binding_text(static_cast<Config::Action>(i)));
            outln("[Log]");
            outln("Level={}", Config::log().level.is_empty() ? "error"sv : Config::log().level.view());
        }
        for (auto const& diagnostic : Config::diagnostics())
            warnln("sde-config: {}", diagnostic);
        if (!Config::diagnostics().is_empty())
            return 1;
        if (check_config)
            outln("sde-config: {} OK", Config::file_path());
        return 0;
    }

    TRY(initialize_gfx());

    auto server = make<Server>();
    if (auto result = initialize(*server); result.is_error()) {
        warnln("WindowServer: {}", result.error());
        return 1;
    }

    auto socket = add_socket(*server, socket_name.to_byte_string());
    if (socket.is_empty()) {
        warnln("WindowServer: failed to create a Wayland socket");
        return 1;
    }

    if (!wlr_backend_start(server->backend)) {
        warnln("WindowServer: failed to start the backend");
        wl_display_destroy(server->display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket.characters(), true);
    dbgln("WindowServer: running on WAYLAND_DISPLAY={}", socket);

    // Publish the configured xkb layout to clients in the standard
    // XKB_DEFAULT_* variables, so a client that wants the active layout (e.g. the
    // Keymap applet, through LibKeyboard) can read it. The keymap itself still
    // travels to clients over wl_keyboard.
    auto const& keyboard = Config::input().keyboard;
    if (!keyboard.layouts.is_empty())
        setenv("XKB_DEFAULT_LAYOUT", keyboard.layouts.characters(), true);
    if (!keyboard.variant.is_empty())
        setenv("XKB_DEFAULT_VARIANT", keyboard.variant.characters(), true);

    // The WindowServer serves the WindowManagerServer protocol (the shell's
    // Taskbar protocol), just like SerenityOS's WindowServer does. The socket
    // path can be overridden so tests can run isolated from a live session.
    ByteString wm_portal { "/tmp/portal/wm" };
    if (auto const* override_path = getenv("SDE_WM_PORTAL_PATH"); override_path && *override_path)
        wm_portal = override_path;
    // Ensure the parent directory exists: /tmp/portal is not guaranteed to survive a
    // tmp-clean, and an override may name a fresh directory. (Previously this was
    // only done for the default path, so a session that exported SDE_WM_PORTAL_PATH
    // =/tmp/portal/wm skipped it and listen() failed - leaving a login with applets
    // but no Taskbar/desktop, as the WM clients could not connect.)
    if (auto directory = LexicalPath(wm_portal).dirname().to_byte_string(); !directory.is_empty())
        (void)Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes);
    ::unlink(wm_portal.characters());
    auto wm_server = Core::LocalServer::construct();
    if (!wm_server->listen(wm_portal))
        warnln("WindowServer: could not listen on {}", wm_portal);
    // Let the window manager call back into the compositor's focus/menu code.
    WindowManager::set_host_hooks({ focus_toplevel, open_window_menu, primary_usable_area, toplevel_report_position, toplevel_begin_interactive, current_palette, on_workspace_changed, set_toplevel_minimized, hook_focus_x11, hook_set_minimized_x11 });
    WindowSwitcher::set_host_hooks({ current_palette, focus_toplevel, WindowManager::switch_workspace, overlay_shadow_hook, toplevel_update_frame, hook_focus_x11, hook_set_minimized_x11, x11_update_frame });
    AppletManager::set_host_hooks({ WindowManager::tell_wms_applet_area_size_changed, primary_usable_area, layer_report_position });
    // Bring the workspace grid up from the config, and follow later edits.
    WindowManager::load_workspaces(*server);
    Config::watch([&] {
        WindowManager::load_workspaces(*server);
        // R6 HiDPI: a live [Output] DefaultScale edit re-scales the outputs.
        apply_output_scale(*server);
        // A live [Applet] Order edit re-lays out the applet area.
        AppletManager::relayout(*server);
    });
    // R4.5: apply monitor-layout edits (screenlayout.ini) to the live outputs.
    Config::watch_outputs([&] { apply_loaded_screen_layout(*server); });
    // R4.6: apply input-setting edits (input.ini) to the live devices.
    Config::watch_input([&] { apply_input_settings(*server); });
    // R5: pick up app-requested global shortcuts (global-shortcuts.ini).
    Config::watch_global_shortcuts([&] { });
    apply_input_settings(*server);
    wm_server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> socket) {
        WindowManager::accept_connection(*server, move(socket));
    };

    // Start the shell only now that the WindowManagerServer is listening and wired:
    // the Taskbar, WorkspacePicker and Keymap connect to /tmp/portal/wm at startup
    // and abort if the connect fails, so spawning them before listen() races on a
    // real login (the applets that don't use the WM connection were unaffected).
    spawn_startup_command(startup_command);

    // One event loop for both Wayland events (via a notifier) and the in-process
    // WindowManagerServer IPC connections.
    Core::EventLoop loop;
    auto* wayland_loop = wl_display_get_event_loop(server->display);
    auto display_notifier = Core::Notifier::construct(wl_event_loop_get_fd(wayland_loop), Core::Notifier::Type::Read);
    display_notifier->on_activation = [&] {
        wl_event_loop_dispatch(wayland_loop, 0);
        wl_display_flush_clients(server->display);
    };

    watch_theme_broker(*server);
    // R4.9: apply the persisted theme ([Theme]) at startup; the broker (a live
    // choice) then takes over.
    if (!Config::theme().path.is_empty())
        apply_theme_path(*server, Config::theme().path);
    // R5 accessibility (docs/29-accessibility.md): high contrast overrides whatever
    // theme is configured -- for the compositor's own chrome here, and for every
    // client through the broker.
    if (Config::accessibility().high_contrast) {
        auto const contrast_theme = high_contrast_theme_path();
        apply_theme_path(*server, contrast_theme);
        publish_theme_to_broker(contrast_theme);
        dbgln("WindowServer: high contrast on; forced '{}'", contrast_theme);
    }
    watch_fonts_broker(*server);
    apply_cursor_state(*server);
    watch_cursor_state(*server);
    watch_wallpaper_state(*server);

    // R6/N4: a liveness heartbeat for the session supervisor's watchdog. A wedged
    // event loop stops updating the file, so the supervisor can restart the
    // session instead of hanging forever. Off unless SDE_HEARTBEAT_FILE is set
    // (the session supervisor sets it), so nested/headless runs are unaffected.
    if (auto const* heartbeat = getenv("SDE_HEARTBEAT_FILE"); heartbeat && *heartbeat) {
        ByteString heartbeat_path { heartbeat };
        // A repeating timer runs on the event loop, so a late callback means the
        // loop was blocked — which is what the session supervisor's watchdog
        // restarts the compositor for ("compositor heartbeat stale"). Say so, with
        // how late and how many frames ago, so the next wedge is diagnosable from
        // the log rather than inferred from the supervisor's side.
        static time_t last_beat;
        auto beat = [heartbeat_path] {
            auto const now = ::time(nullptr);
            if (last_beat != 0 && now - last_beat > 2) {
                dbgln("WindowServer: WARNING: event loop stalled for {}s (the session watchdog restarts at 10s)",
                    static_cast<long>(now - last_beat));
            }
            last_beat = now;
            // The heartbeat is a synchronous file write on the loop it certifies, so
            // if writing it stalls — I/O contention on a nearly-full filesystem is
            // the obvious way — the compositor goes stale *because of* the thing
            // meant to show it is alive, and the supervisor's only evidence is
            // "heartbeat stale". Time it, and say so when it is materially slow.
            auto const started = monotonic_time_msec();
            auto file = Core::File::open(heartbeat_path, Core::File::OpenMode::Write);
            if (file.is_error())
                return;
            auto text = ByteString::number(static_cast<long>(now));
            (void)file.value()->write_until_depleted(text.bytes());
            auto const elapsed_ms = monotonic_time_msec() - started;
            if (elapsed_ms > 500)
                dbgln("WindowServer: WARNING: writing the liveness heartbeat took {}ms", elapsed_ms);
        };
        beat();
        static RefPtr<Core::Timer> heartbeat_timer;
        heartbeat_timer = Core::Timer::create_repeating(1000, [beat] { beat(); });
        heartbeat_timer->start();
    }

    // R6: the performance meter's report (see the meter above output_frame()).
    if (auto const* stats_path = getenv("SDE_PERF_STATS"); stats_path && *stats_path) {
        ByteString path { stats_path };
        bool const log = getenv("SDE_PERF_LOG") && *getenv("SDE_PERF_LOG");
        s_perf.started_ms = perf_now_ms();
        static RefPtr<Core::Timer> perf_timer;
        auto* perf_server = server.ptr();
        perf_timer = Core::Timer::create_repeating(1000, [path, log, perf_server] {
            // Resident memory: current, high-water, and the change since the last
            // report, so growth (a leak) is visible in the same stream as latency.
            auto const memory = perf_process_memory(getpid());
            auto const rss = memory.rss_kb;
            s_perf.pss_kb = memory.pss_kb;
            s_perf.rss_delta_kb = s_perf.rss_previous_kb == 0 ? 0 : static_cast<i64>(rss) - static_cast<i64>(s_perf.rss_previous_kb);
            s_perf.rss_previous_kb = rss;
            s_perf.rss_kb = rss;
            s_perf.rss_high_kb = max(s_perf.rss_high_kb, rss);
            if (auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !file.is_error())
                (void)file.value()->write_until_depleted(perf_summary(*perf_server));
            if (log)
                dbgln("WindowServer: perf: frames={} fps={:.1} frame p50/p95/max={:.2}/{:.2}/{:.2} ms slow={} input->present p50/p95={:.2}/{:.2} ms",
                    s_perf.frames, static_cast<double>(s_perf.frames) / max(0.001, (perf_now_ms() - s_perf.started_ms) / 1000.0),
                    s_perf.frame_ms.percentile(0.5), s_perf.frame_ms.percentile(0.95), s_perf.frame_ms.maximum, s_perf.slow_frames,
                    s_perf.input_to_present_ms.percentile(0.5), s_perf.input_to_present_ms.percentile(0.95));
        });
        perf_timer->start();
    }

    int result = loop.exec();
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);
    return result;
}
