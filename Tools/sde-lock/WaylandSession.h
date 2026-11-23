/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "LockScreen.h"

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

namespace SDELock {

// Owns the Wayland connection, ext-session-lock-v1 lifecycle, per-output
// double-buffered wl_shm pools, cursor surface, and pointer/keyboard listeners.
class WaylandSession {
public:
    explicit WaylandSession(LockRenderer const& renderer);
    ~WaylandSession();

    WaylandSession(WaylandSession const&) = delete;
    WaylandSession& operator=(WaylandSession const&) = delete;

    ErrorOr<void> connect();
    void lock();
    void unlock();

    // Proper Wayland prepare_read -> poll -> read_events -> dispatch cycle.
    // Returns false if the Wayland connection closed or errored.
    bool poll_and_dispatch(Optional<int> auth_fd, int timeout_ms, bool& auth_ready);

    void mark_all_dirty(bool full_redraw = false);
    void render_dirty(UiState const& state);

    bool is_locked() const { return m_locked; }

    Function<void()> on_locked;
    Function<void()> on_finished;
    Function<void(uint32_t format, int fd, uint32_t size)> on_keymap;
    Function<void(uint32_t keycode, bool pressed)> on_key;
    Function<void(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)> on_modifiers;
    Function<void(int32_t rate, int32_t delay)> on_repeat_info;
    Function<void(bool hovered)> on_button_hover_changed;
    Function<void()> on_unlock_clicked;
    Function<void()> on_pointer_activity;

    // Public static Wayland callbacks (referenced by listener structs).
    static void registry_global(void* data, wl_registry*, uint32_t name, char const* interface, uint32_t version);
    static void registry_global_remove(void* data, wl_registry*, uint32_t name);
    static void seat_capabilities(void* data, wl_seat*, uint32_t capabilities);
    static void seat_name(void* data, wl_seat*, char const* name);
    static void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size);
    static void keyboard_enter(void* data, wl_keyboard*, uint32_t serial, wl_surface*, wl_array* keys);
    static void keyboard_leave(void* data, wl_keyboard*, uint32_t serial, wl_surface*);
    static void keyboard_key(void* data, wl_keyboard*, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    static void keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay);
    static void pointer_enter(void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y);
    static void pointer_leave(void* data, wl_pointer*, uint32_t serial, wl_surface*);
    static void pointer_motion(void* data, wl_pointer*, uint32_t time, wl_fixed_t x, wl_fixed_t y);
    static void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
    static void pointer_axis(void* data, wl_pointer*, uint32_t time, uint32_t axis, wl_fixed_t value);
    static void pointer_frame(void* data, wl_pointer*);
    static void pointer_axis_source(void* data, wl_pointer*, uint32_t axis_source);
    static void pointer_axis_stop(void* data, wl_pointer*, uint32_t time, uint32_t axis);
    static void pointer_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete);
    static void pointer_axis_value120(void* data, wl_pointer*, uint32_t axis, int32_t value120);
    static void pointer_axis_relative_direction(void* data, wl_pointer*, uint32_t axis, uint32_t direction);
    static void pointer_warp(void* data, wl_pointer*, wl_fixed_t x, wl_fixed_t y);
    static void lock_locked(void* data, ext_session_lock_v1*);
    static void lock_finished(void* data, ext_session_lock_v1*);
    static void lock_surface_configure(void* data, ext_session_lock_surface_v1*, uint32_t serial, uint32_t width, uint32_t height);
    static void output_geometry(void* data, wl_output*, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t subpixel, char const* make, char const* model, int32_t transform);
    static void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t refresh);
    static void output_done(void* data, wl_output*);
    static void output_scale(void* data, wl_output*, int32_t factor);
    static void output_name(void* data, wl_output*, char const* name);
    static void output_description(void* data, wl_output*, char const* description);
    static void buffer_release(void* data, wl_buffer*);

private:
    struct ShmSlot {
        wl_buffer* buffer { nullptr };
        void* data { nullptr };
        size_t size { 0 };
        Gfx::IntSize pixel_size;
        bool busy { false };
    };

    struct Output {
        WaylandSession* session { nullptr };
        wl_output* output { nullptr };
        wl_surface* surface { nullptr };
        ext_session_lock_surface_v1* lock_surface { nullptr };
        Gfx::IntSize logical_size;
        int scale { 1 };
        bool configured { false };
        bool full_dirty { true };
        bool panel_dirty { true };
        RefPtr<Gfx::Bitmap> cached_background;
        RefPtr<Gfx::Bitmap> framebuffer;
        ShmSlot slots[2];
        uint32_t registry_name { 0 };
    };

    void create_lock_surface(Output&);
    void destroy_output(Output*);
    void destroy_slot(ShmSlot&);
    bool ensure_slot(ShmSlot&, Gfx::IntSize pixel_size);
    void ensure_cursor(int scale);
    Output* output_for_surface(wl_surface*);
    Output* effective_active_output() const;
    void render_output(Output&, UiState const&);
    void commit_output(Output&, Gfx::IntRect damage_rect);
    bool flush_display();

    LockRenderer const& m_renderer;

    wl_display* m_display { nullptr };
    wl_registry* m_registry { nullptr };
    wl_compositor* m_compositor { nullptr };
    wl_shm* m_shm { nullptr };
    wl_seat* m_seat { nullptr };
    wl_keyboard* m_keyboard { nullptr };
    wl_pointer* m_pointer { nullptr };
    ext_session_lock_manager_v1* m_lock_manager { nullptr };
    ext_session_lock_v1* m_lock { nullptr };

    wl_surface* m_cursor_surface { nullptr };
    ShmSlot m_cursor_slot;
    int m_cursor_scale { 0 };

    Vector<Output*> m_outputs;
    Output* m_active_output { nullptr };
    Output* m_hover_output { nullptr };
    Gfx::IntPoint m_pointer_position;
    bool m_button_hovered { false };
    bool m_locked { false };
    bool m_need_flush { false };
};

}
