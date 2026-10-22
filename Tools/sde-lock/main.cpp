/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-lock: the SDE's native session locker.
//
// A plain ext-session-lock-v1 client, as docs/11 §10 requires: no SDE-specific
// protocols, so it locks an SDE session and any other conformant compositor
// (sway, Hyprland, KWin). The UI is drawn with LibGfx onto wl_shm buffers; it is
// deliberately *not* a LibGUI app, because LibGUI goes through LibWM, which speaks
// the SDE's own window protocol and would tie the locker to this desktop.
//
// The look borrows from XFCE's lock screen: the session wallpaper, dimmed, with a
// centred panel holding the clock, date, user avatar/name, a password field and an
// Unlock button.

#include "Auth.h"
#include "LockScreen.h"

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibCore/DateTime.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "ext-session-lock-v1-client-protocol.h"

namespace {

using namespace SDELock;

constexpr uint32_t BTN_LEFT = 0x110;
constexpr int caret_blink_ms = 500;
constexpr int auth_cooldown_ms = 1000;

volatile sig_atomic_t s_quit = 0;

void on_terminate_signal(int)
{
    s_quit = 1;
}

long monotonic_ms()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

ByteString resource_root()
{
    if (auto const* env = getenv("SERENITY_RES_ROOT"); env && *env)
        return ByteString(env);
    if (access("/usr/share/serenity/res/fonts", R_OK) == 0)
        return ByteString("/usr/share/serenity/res");
    return {};
}

struct App;

struct Output {
    App* app { nullptr };
    wl_output* output { nullptr };
    wl_surface* surface { nullptr };
    ext_session_lock_surface_v1* lock_surface { nullptr };
    Gfx::IntSize logical_size;
    int scale { 1 };
    bool configured { false };
    bool dirty { true };
    RefPtr<Gfx::Bitmap> bitmap;
    uint32_t registry_name { 0 };
};

struct ShmBuffer {
    wl_buffer* buffer { nullptr };
    void* data { nullptr };
    size_t size { 0 };
};

class App {
public:
    App(ByteString username, ByteString pam_service)
        : m_username(move(username))
        , m_authenticator(m_pam_service_or_default(pam_service), m_username)
    {
    }

    ~App()
    {
        for (auto* output : m_outputs) {
            if (output->lock_surface)
                ext_session_lock_surface_v1_destroy(output->lock_surface);
            if (output->surface)
                wl_surface_destroy(output->surface);
            if (output->output)
                wl_output_destroy(output->output);
            delete output;
        }
        if (m_lock)
            ext_session_lock_v1_destroy(m_lock);
        if (m_pointer)
            wl_pointer_destroy(m_pointer);
        if (m_keyboard)
            wl_keyboard_destroy(m_keyboard);
        if (m_seat)
            wl_seat_destroy(m_seat);
        if (m_lock_manager)
            ext_session_lock_manager_v1_destroy(m_lock_manager);
        if (m_shm)
            wl_shm_destroy(m_shm);
        if (m_compositor)
            wl_compositor_destroy(m_compositor);
        if (m_registry)
            wl_registry_destroy(m_registry);
        if (m_display)
            wl_display_disconnect(m_display);
        if (m_xkb_state)
            xkb_state_unref(m_xkb_state);
        if (m_xkb_keymap)
            xkb_keymap_unref(m_xkb_keymap);
        if (m_xkb_context)
            xkb_context_unref(m_xkb_context);
    }

    ErrorOr<int> run();

    // The Wayland listener tables are namespace-scope constants, so the callbacks
    // they name have to be reachable from outside the class.
    static void registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version);
    static void registry_global_remove(void* data, wl_registry* registry, uint32_t name);
    static void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities);
    static void seat_name(void* data, wl_seat* seat, char const* name);
    static void keyboard_keymap(void* data, wl_keyboard* keyboard, uint32_t format, int fd, uint32_t size);
    static void keyboard_enter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface, wl_array* keys);
    static void keyboard_leave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface);
    static void keyboard_key(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    static void keyboard_modifiers(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    static void keyboard_repeat_info(void* data, wl_keyboard* keyboard, int32_t rate, int32_t delay);
    static void pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y);
    static void pointer_leave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface);
    static void pointer_motion(void* data, wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y);
    static void pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
    static void pointer_axis(void* data, wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value);
    static void lock_locked(void* data, ext_session_lock_v1* lock);
    static void lock_finished(void* data, ext_session_lock_v1* lock);
    static void lock_surface_configure(void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height);
    static void output_geometry(void* data, wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, char const* make, char const* model, int32_t transform);
    static void output_mode(void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
    static void output_done(void* data, wl_output* output);
    static void output_scale(void* data, wl_output* output, int32_t factor);
    static void output_name(void* data, wl_output* output, char const* name);
    static void output_description(void* data, wl_output* output, char const* description);
    static void buffer_release(void* data, wl_buffer* buffer);

private:
    static ByteString m_pam_service_or_default(ByteString const& service)
    {
        if (!service.is_empty())
            return service;
        if (auto const* env = getenv("SDE_LOCK_PAM_SERVICE"); env && *env)
            return ByteString(env);
        return ByteString("login");
    }

    void create_lock_surface(Output&);
    Output* output_for_surface(wl_surface*);
    void render_dirty();
    void render_output(Output&);
    void commit(Output&, Gfx::Bitmap&);
    void mark_all_dirty();
    void update_clock(bool force = false);
    void update_caret();
    void submit();
    void on_auth_result();
    void clear_password();
    void unlock();
    void handle_key(uint32_t keycode, uint32_t state);

    ByteString m_username;
    Authenticator m_authenticator;

    wl_display* m_display { nullptr };
    wl_registry* m_registry { nullptr };
    wl_compositor* m_compositor { nullptr };
    wl_shm* m_shm { nullptr };
    wl_seat* m_seat { nullptr };
    wl_keyboard* m_keyboard { nullptr };
    wl_pointer* m_pointer { nullptr };
    ext_session_lock_manager_v1* m_lock_manager { nullptr };
    ext_session_lock_v1* m_lock { nullptr };

    xkb_context* m_xkb_context { nullptr };
    xkb_keymap* m_xkb_keymap { nullptr };
    xkb_state* m_xkb_state { nullptr };

    Vector<Output*> m_outputs;
    LockRenderer m_renderer;
    SDELock::UiState m_state;

    Vector<u32> m_password;
    ByteString m_message;
    bool m_authenticating { false };
    bool m_failed { false };
    bool m_caps_lock { false };
    bool m_caret_visible { true };
    bool m_caret_on { false };
    bool m_button_hovered { false };
    bool m_locked { false };
    bool m_running { true };
    long m_last_caret_toggle { 0 };
    long m_cooldown_until { 0 };
    Output* m_hover_output { nullptr };
    Gfx::IntPoint m_pointer_position;
};

const wl_registry_listener s_registry_listener = {
    .global = App::registry_global,
    .global_remove = App::registry_global_remove,
};

const wl_seat_listener s_seat_listener = {
    .capabilities = App::seat_capabilities,
    .name = App::seat_name,
};

const wl_keyboard_listener s_keyboard_listener = {
    .keymap = App::keyboard_keymap,
    .enter = App::keyboard_enter,
    .leave = App::keyboard_leave,
    .key = App::keyboard_key,
    .modifiers = App::keyboard_modifiers,
    .repeat_info = App::keyboard_repeat_info,
};

const wl_pointer_listener s_pointer_listener = {
    .enter = App::pointer_enter,
    .leave = App::pointer_leave,
    .motion = App::pointer_motion,
    .button = App::pointer_button,
    .axis = App::pointer_axis,
};

const ext_session_lock_v1_listener s_lock_listener = {
    .locked = App::lock_locked,
    .finished = App::lock_finished,
};

const ext_session_lock_surface_v1_listener s_lock_surface_listener = {
    .configure = App::lock_surface_configure,
};

const wl_output_listener s_output_listener = {
    .geometry = App::output_geometry,
    .mode = App::output_mode,
    .done = App::output_done,
    .scale = App::output_scale,
    .name = App::output_name,
    .description = App::output_description,
};

const wl_buffer_listener s_buffer_listener = {
    .release = App::buffer_release,
};

void App::registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    auto& app = *static_cast<App*>(data);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app.m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, min(version, 4u)));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app.m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app.m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 7u)));
        wl_seat_add_listener(app.m_seat, &s_seat_listener, &app);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        auto* output = new Output;
        output->app = &app;
        output->registry_name = name;
        output->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, min(version, 4u)));
        wl_output_add_listener(output->output, &s_output_listener, output);
        app.m_outputs.append(output);
        if (app.m_lock)
            app.create_lock_surface(*output);
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        app.m_lock_manager = static_cast<ext_session_lock_manager_v1*>(wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
    }
}

void App::registry_global_remove(void* data, wl_registry* registry, uint32_t name)
{
    (void)registry;
    auto& app = *static_cast<App*>(data);

    for (size_t i = 0; i < app.m_outputs.size(); ++i) {
        auto* output = app.m_outputs[i];
        if (output->registry_name != name)
            continue;
        if (app.m_hover_output == output)
            app.m_hover_output = nullptr;
        if (output->lock_surface)
            ext_session_lock_surface_v1_destroy(output->lock_surface);
        if (output->surface)
            wl_surface_destroy(output->surface);
        if (output->output)
            wl_output_destroy(output->output);
        app.m_outputs.remove(i);
        delete output;
        return;
    }
}

void App::seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities)
{
    auto& app = *static_cast<App*>(data);

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app.m_keyboard) {
        app.m_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app.m_keyboard, &s_keyboard_listener, &app);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app.m_pointer) {
        app.m_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app.m_pointer, &s_pointer_listener, &app);
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app.m_keyboard) {
        wl_keyboard_destroy(app.m_keyboard);
        app.m_keyboard = nullptr;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app.m_pointer) {
        wl_pointer_destroy(app.m_pointer);
        app.m_pointer = nullptr;
    }
}

void App::seat_name(void* data, wl_seat* seat, char const* name)
{
    (void)data;
    (void)seat;
    (void)name;
}

void App::keyboard_keymap(void* data, wl_keyboard* keyboard, uint32_t format, int fd, uint32_t size)
{
    (void)keyboard;
    auto& app = *static_cast<App*>(data);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        return;

    auto* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED)
        return;

    app.m_xkb_keymap = xkb_keymap_new_from_string(app.m_xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);

    if (!app.m_xkb_keymap)
        return;

    if (app.m_xkb_state)
        xkb_state_unref(app.m_xkb_state);
    app.m_xkb_state = xkb_state_new(app.m_xkb_keymap);
}

void App::keyboard_enter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface, wl_array* keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    dbgln("sde-lock: keyboard ready");
}

void App::keyboard_leave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

void App::keyboard_key(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    static_cast<App*>(data)->handle_key(key, state);
}

void App::keyboard_modifiers(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    auto& app = *static_cast<App*>(data);
    if (!app.m_xkb_state)
        return;

    xkb_state_update_mask(app.m_xkb_state, depressed, latched, locked, 0, 0, group);
    bool const caps = xkb_state_mod_name_is_active(app.m_xkb_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
    if (caps != app.m_caps_lock) {
        app.m_caps_lock = caps;
        app.mark_all_dirty();
    }
}

void App::keyboard_repeat_info(void* data, wl_keyboard* keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

void App::pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)serial;
    auto& app = *static_cast<App*>(data);
    app.m_hover_output = app.output_for_surface(surface);
    app.m_pointer_position = { static_cast<int>(wl_fixed_to_double(x)), static_cast<int>(wl_fixed_to_double(y)) };
}

void App::pointer_leave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    auto& app = *static_cast<App*>(data);
    app.m_hover_output = nullptr;
    if (app.m_button_hovered) {
        app.m_button_hovered = false;
        app.mark_all_dirty();
    }
}

void App::pointer_motion(void* data, wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    auto& app = *static_cast<App*>(data);
    app.m_pointer_position = { static_cast<int>(wl_fixed_to_double(x)), static_cast<int>(wl_fixed_to_double(y)) };

    bool hovered = false;
    if (app.m_hover_output && app.m_hover_output->configured) {
        auto const layout = app.m_renderer.layout_for(app.m_hover_output->logical_size);
        hovered = layout.button.contains(app.m_pointer_position);
    }
    if (hovered != app.m_button_hovered) {
        app.m_button_hovered = hovered;
        app.mark_all_dirty();
    }
}

void App::pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    auto& app = *static_cast<App*>(data);
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    if (!app.m_hover_output || !app.m_hover_output->configured)
        return;

    auto const layout = app.m_renderer.layout_for(app.m_hover_output->logical_size);
    if (layout.button.contains(app.m_pointer_position))
        app.submit();
}

void App::pointer_axis(void* data, wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

void App::lock_locked(void* data, ext_session_lock_v1* lock)
{
    (void)lock;
    auto& app = *static_cast<App*>(data);
    app.m_locked = true;
    app.mark_all_dirty();
    dbgln("sde-lock: locked");
}

void App::lock_finished(void* data, ext_session_lock_v1* lock)
{
    (void)lock;
    // Another client took the session over; there is nothing left to draw.
    static_cast<App*>(data)->m_running = false;
}

void App::lock_surface_configure(void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height)
{
    auto& output = *static_cast<Output*>(data);
    ext_session_lock_surface_v1_ack_configure(surface, serial);

    output.logical_size = { static_cast<int>(width), static_cast<int>(height) };
    output.configured = true;
    output.dirty = true;
}

void App::output_geometry(void* data, wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, char const* make, char const* model, int32_t transform)
{
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

void App::output_mode(void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

void App::output_done(void* data, wl_output* output)
{
    (void)data;
    (void)output;
}

void App::output_scale(void* data, wl_output* output, int32_t factor)
{
    (void)output;
    auto& entry = *static_cast<Output*>(data);
    if (factor <= 0 || entry.scale == factor)
        return;
    entry.scale = factor;
    entry.bitmap = nullptr;
    entry.dirty = true;
}

void App::output_name(void* data, wl_output* output, char const* name)
{
    (void)data;
    (void)output;
    (void)name;
}

void App::output_description(void* data, wl_output* output, char const* description)
{
    (void)data;
    (void)output;
    (void)description;
}

void App::buffer_release(void* data, wl_buffer* buffer)
{
    auto* shm_buffer = static_cast<ShmBuffer*>(data);
    munmap(shm_buffer->data, shm_buffer->size);
    wl_buffer_destroy(buffer);
    delete shm_buffer;
}

void App::create_lock_surface(Output& output)
{
    if (!m_lock || !m_compositor || output.surface)
        return;

    output.surface = wl_compositor_create_surface(m_compositor);
    output.lock_surface = ext_session_lock_v1_get_lock_surface(m_lock, output.surface, output.output);
    ext_session_lock_surface_v1_add_listener(output.lock_surface, &s_lock_surface_listener, &output);
}

Output* App::output_for_surface(wl_surface* surface)
{
    for (auto* output : m_outputs) {
        if (output->surface == surface)
            return output;
    }
    return nullptr;
}

void App::mark_all_dirty()
{
    for (auto* output : m_outputs)
        output->dirty = true;
}

void App::update_clock(bool force)
{
    auto now = Core::DateTime::now();
    auto clock = now.to_byte_string("%H:%M"sv);
    auto date = now.to_byte_string("%A, %d %B %Y"sv);

    if (!force && clock == m_state.clock && date == m_state.date)
        return;
    m_state.clock = move(clock);
    m_state.date = move(date);
    mark_all_dirty();
}

void App::update_caret()
{
    long const now = monotonic_ms();
    if (now - m_last_caret_toggle < caret_blink_ms)
        return;
    m_last_caret_toggle = now;
    m_caret_on = !m_caret_on;
    m_state.caret_visible = m_caret_on && !m_authenticating;
    mark_all_dirty();
}

void App::clear_password()
{
    if (!m_password.is_empty())
        explicit_bzero(m_password.data(), m_password.size() * sizeof(u32));
    m_password.clear();
    m_state.password_length = 0;
}

void App::submit()
{
    if (m_authenticating || m_password.is_empty())
        return;
    long const now = monotonic_ms();
    if (now < m_cooldown_until)
        return;

    StringBuilder builder;
    for (auto codepoint : m_password)
        builder.append_code_point(codepoint);
    auto password = builder.to_string_without_validation();
    clear_password();

    m_authenticating = true;
    m_failed = false;
    m_message = {};
    m_state.authenticating = true;
    m_state.failed = false;
    m_state.message = {};
    m_state.caret_visible = false;
    mark_all_dirty();

    if (!m_authenticator.begin(password))
        m_authenticating = false;
    password = {};
}

void App::on_auth_result()
{
    ByteString message;
    auto const result = m_authenticator.take_result(message);

    m_authenticating = false;
    m_state.authenticating = false;

    if (result == SDELock::AuthResult::Success) {
        dbgln("sde-lock: authenticated");
        m_running = false;
        unlock();
        return;
    }

    dbgln("sde-lock: authentication {}: {}", result == SDELock::AuthResult::Failure ? "failed" : "error", message);

    m_failed = true;
    m_message = message.is_empty() ? ByteString("Incorrect password") : message;
    m_state.failed = true;
    m_state.message = m_message;
    m_cooldown_until = monotonic_ms() + auth_cooldown_ms;
    mark_all_dirty();
}

void App::unlock()
{
    if (m_lock) {
        ext_session_lock_v1_unlock_and_destroy(m_lock);
        m_lock = nullptr;
        if (m_display)
            wl_display_roundtrip(m_display);
    }
    m_running = false;
}

void App::handle_key(uint32_t keycode, uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    if (m_authenticating || !m_xkb_state)
        return;

    auto const sym = xkb_state_key_get_one_sym(m_xkb_state, keycode + 8);
    bool const ctrl = xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED) > 0;
    bool const alt = xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_DEPRESSED) > 0;

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        submit();
        return;
    }
    if (sym == XKB_KEY_BackSpace) {
        if (!m_password.is_empty())
            m_password.take_last();
        m_state.password_length = m_password.size();
        m_state.caret_visible = true;
        m_caret_on = true;
        m_last_caret_toggle = monotonic_ms();
        mark_all_dirty();
        return;
    }
    if (sym == XKB_KEY_Escape || (ctrl && sym == XKB_KEY_u)) {
        clear_password();
        mark_all_dirty();
        return;
    }
    if (sym == XKB_KEY_Delete) {
        clear_password();
        mark_all_dirty();
        return;
    }

    // Never turn Ctrl/Alt chords into password text.
    if (ctrl || alt)
        return;

    char buffer[64];
    int const length = xkb_state_key_get_utf8(m_xkb_state, keycode + 8, buffer, sizeof(buffer));
    if (length <= 0)
        return;

    bool changed = false;
    Utf8View view { StringView { buffer, static_cast<size_t>(length) } };
    for (auto codepoint : view) {
        if (codepoint < 0x20 || codepoint == 0x7f)
            continue;
        m_password.append(codepoint);
        changed = true;
    }
    if (changed) {
        m_state.password_length = m_password.size();
        m_state.caret_visible = true;
        m_caret_on = true;
        m_last_caret_toggle = monotonic_ms();
        mark_all_dirty();
    }
}

void App::render_output(Output& output)
{
    if (!output.configured || output.logical_size.is_empty())
        return;

    Gfx::IntSize const pixel_size {
        output.logical_size.width() * output.scale,
        output.logical_size.height() * output.scale,
    };

    if (!output.bitmap || output.bitmap->size() != pixel_size) {
        auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, pixel_size);
        if (bitmap.is_error())
            return;
        output.bitmap = bitmap.release_value();
    }

    m_state.username = m_username;
    m_renderer.render(*output.bitmap, m_state);
    commit(output, *output.bitmap);
    output.dirty = false;
}

void App::commit(Output& output, Gfx::Bitmap& bitmap)
{
    int const stride = bitmap.width() * 4;
    size_t const bytes = static_cast<size_t>(stride) * bitmap.height();

    int fd = memfd_create("sde-lock", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        close(fd);
        return;
    }

    auto* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return;
    }

    for (int y = 0; y < bitmap.height(); ++y)
        memcpy(static_cast<u8*>(data) + static_cast<size_t>(y) * stride, bitmap.scanline_u8(y), stride);

    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int32_t>(bytes));
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, bitmap.width(), bitmap.height(), stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    auto* shm_buffer = new ShmBuffer { buffer, data, bytes };
    wl_buffer_add_listener(buffer, &s_buffer_listener, shm_buffer);

    wl_surface_set_buffer_scale(output.surface, output.scale);
    wl_surface_attach(output.surface, buffer, 0, 0);
    wl_surface_damage_buffer(output.surface, 0, 0, bitmap.width(), bitmap.height());
    wl_surface_commit(output.surface);
}

void App::render_dirty()
{
    for (auto* output : m_outputs) {
        if (output->dirty)
            render_output(*output);
    }
}

ErrorOr<int> App::run()
{
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        warnln("sde-lock: cannot connect to a Wayland compositor");
        return 1;
    }

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registry_listener, this);
    if (wl_display_roundtrip(m_display) < 0)
        return 1;
    if (wl_display_roundtrip(m_display) < 0)
        return 1;

    if (!m_compositor || !m_shm) {
        warnln("sde-lock: the compositor is missing wl_compositor/wl_shm");
        return 1;
    }
    if (!m_lock_manager) {
        warnln("sde-lock: the compositor does not support ext-session-lock-v1");
        return 1;
    }

    m_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    TRY(m_renderer.initialize(resource_root(), m_username));
    m_renderer.set_wallpaper(LockRenderer::load_wallpaper(LockRenderer::default_wallpaper_path()));

    m_state.username = m_username;
    update_clock(true);
    m_state.caret_visible = true;
    m_caret_on = true;
    m_last_caret_toggle = monotonic_ms();

    m_lock = ext_session_lock_manager_v1_lock(m_lock_manager);
    ext_session_lock_v1_add_listener(m_lock, &s_lock_listener, this);
    for (auto* output : m_outputs)
        create_lock_surface(*output);
    wl_display_flush(m_display);

    while (m_running) {
        if (s_quit) {
            unlock();
            break;
        }

        wl_display_dispatch_pending(m_display);

        struct pollfd fds[2];
        int count = 1;
        fds[0] = { wl_display_get_fd(m_display), POLLIN, 0 };
        if (auto auth_fd = m_authenticator.result_fd(); auth_fd.has_value()) {
            fds[1] = { *auth_fd, POLLIN, 0 };
            count = 2;
        }

        int const ready = poll(fds, count, 250);
        if (ready > 0) {
            if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
                if (wl_display_dispatch(m_display) < 0)
                    break;
            }
            if (count > 1 && (fds[1].revents & POLLIN))
                on_auth_result();
        }

        update_clock();
        update_caret();
        render_dirty();
        wl_display_flush(m_display);
    }

    return 0;
}

void usage(char const* program)
{
    outln("Usage: {} [options]", program);
    outln("  --pam-service NAME   PAM service to authenticate against (default: login,");
    outln("                       or $SDE_LOCK_PAM_SERVICE)");
    outln("  --username NAME      authenticate as NAME (default: the current user)");
    outln("  --help               show this help");
}

}

int main(int argc, char** argv)
{
    signal(SIGTERM, on_terminate_signal);
    signal(SIGINT, on_terminate_signal);
    signal(SIGPIPE, SIG_IGN);

    ByteString username;
    ByteString pam_service;

    for (int i = 1; i < argc; ++i) {
        StringView argument { argv[i], strlen(argv[i]) };
        if (argument == "--help"sv || argument == "-h"sv) {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--pam-service"sv && i + 1 < argc) {
            pam_service = ByteString(argv[++i]);
            continue;
        }
        if (argument.starts_with("--pam-service="sv)) {
            pam_service = ByteString(argument.substring_view(strlen("--pam-service=")));
            continue;
        }
        if (argument == "--username"sv && i + 1 < argc) {
            username = ByteString(argv[++i]);
            continue;
        }
        if (argument.starts_with("--username="sv)) {
            username = ByteString(argument.substring_view(strlen("--username=")));
            continue;
        }
        warnln("sde-lock: unknown argument '{}'", argument);
        usage(argv[0]);
        return 2;
    }

    if (username.is_empty()) {
        auto* user = getpwuid(getuid());
        if (!user || !user->pw_name) {
            warnln("sde-lock: cannot determine the current user");
            return 1;
        }
        username = ByteString(user->pw_name);
    }

    App app(move(username), move(pam_service));
    auto result = app.run();
    if (result.is_error())
        return 1;
    return result.value();
}
