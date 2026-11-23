/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WaylandSession.h"

#include <AK/Debug.h>
#include <AK/Format.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace SDELock {

namespace {

constexpr uint32_t BTN_LEFT = 0x110;

const wl_registry_listener s_registry_listener = {
    .global = WaylandSession::registry_global,
    .global_remove = WaylandSession::registry_global_remove,
};

const wl_seat_listener s_seat_listener = {
    .capabilities = WaylandSession::seat_capabilities,
    .name = WaylandSession::seat_name,
};

const wl_keyboard_listener s_keyboard_listener = {
    .keymap = WaylandSession::keyboard_keymap,
    .enter = WaylandSession::keyboard_enter,
    .leave = WaylandSession::keyboard_leave,
    .key = WaylandSession::keyboard_key,
    .modifiers = WaylandSession::keyboard_modifiers,
    .repeat_info = WaylandSession::keyboard_repeat_info,
};

const wl_pointer_listener s_pointer_listener = {
    .enter = WaylandSession::pointer_enter,
    .leave = WaylandSession::pointer_leave,
    .motion = WaylandSession::pointer_motion,
    .button = WaylandSession::pointer_button,
    .axis = WaylandSession::pointer_axis,
    .frame = WaylandSession::pointer_frame,
    .axis_source = WaylandSession::pointer_axis_source,
    .axis_stop = WaylandSession::pointer_axis_stop,
    .axis_discrete = WaylandSession::pointer_axis_discrete,
    .axis_value120 = WaylandSession::pointer_axis_value120,
    .axis_relative_direction = WaylandSession::pointer_axis_relative_direction,
    .warp = WaylandSession::pointer_warp,
};

const ext_session_lock_v1_listener s_lock_listener = {
    .locked = WaylandSession::lock_locked,
    .finished = WaylandSession::lock_finished,
};

const ext_session_lock_surface_v1_listener s_lock_surface_listener = {
    .configure = WaylandSession::lock_surface_configure,
};

const wl_output_listener s_output_listener = {
    .geometry = WaylandSession::output_geometry,
    .mode = WaylandSession::output_mode,
    .done = WaylandSession::output_done,
    .scale = WaylandSession::output_scale,
    .name = WaylandSession::output_name,
    .description = WaylandSession::output_description,
};

const wl_buffer_listener s_buffer_listener = {
    .release = WaylandSession::buffer_release,
};

}

WaylandSession::WaylandSession(LockRenderer const& renderer)
    : m_renderer(renderer)
{
}

WaylandSession::~WaylandSession()
{
    for (auto* output : m_outputs)
        destroy_output(output);
    m_outputs.clear();

    destroy_slot(m_cursor_slot);
    if (m_cursor_surface)
        wl_surface_destroy(m_cursor_surface);
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
}

void WaylandSession::destroy_slot(ShmSlot& slot)
{
    if (slot.buffer) {
        wl_buffer_destroy(slot.buffer);
        slot.buffer = nullptr;
    }
    if (slot.data && slot.data != MAP_FAILED) {
        munmap(slot.data, slot.size);
        slot.data = nullptr;
    }
    slot.size = 0;
    slot.pixel_size = {};
    slot.busy = false;
}

void WaylandSession::destroy_output(Output* output)
{
    if (!output)
        return;
    destroy_slot(output->slots[0]);
    destroy_slot(output->slots[1]);
    if (output->lock_surface)
        ext_session_lock_surface_v1_destroy(output->lock_surface);
    if (output->surface)
        wl_surface_destroy(output->surface);
    if (output->output)
        wl_output_destroy(output->output);
    delete output;
}

ErrorOr<void> WaylandSession::connect()
{
    m_display = wl_display_connect(nullptr);
    if (!m_display)
        return Error::from_string_literal("cannot connect to a Wayland compositor");

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registry_listener, this);
    if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0)
        return Error::from_string_literal("Wayland roundtrip failed");

    if (!m_compositor || !m_shm)
        return Error::from_string_literal("the compositor is missing wl_compositor/wl_shm");
    if (!m_lock_manager)
        return Error::from_string_literal("the compositor does not support ext-session-lock-v1");

    return {};
}

void WaylandSession::lock()
{
    if (!m_lock_manager || m_lock)
        return;
    m_lock = ext_session_lock_manager_v1_lock(m_lock_manager);
    ext_session_lock_v1_add_listener(m_lock, &s_lock_listener, this);
    for (auto* output : m_outputs)
        create_lock_surface(*output);
    (void)flush_display();
}

void WaylandSession::unlock()
{
    if (!m_lock)
        return;
    ext_session_lock_v1_unlock_and_destroy(m_lock);
    m_lock = nullptr;
    if (m_display)
        wl_display_roundtrip(m_display);
}

bool WaylandSession::flush_display()
{
    if (!m_display)
        return false;
    while (wl_display_flush(m_display) < 0) {
        if (errno == EAGAIN) {
            m_need_flush = true;
            return true;
        }
        return false;
    }
    m_need_flush = false;
    return true;
}

bool WaylandSession::poll_and_dispatch(Optional<int> auth_fd, int timeout_ms, bool& auth_ready)
{
    auth_ready = false;
    if (!m_display)
        return false;

    while (wl_display_prepare_read(m_display) != 0) {
        if (wl_display_dispatch_pending(m_display) < 0)
            return false;
    }

    if (!flush_display()) {
        wl_display_cancel_read(m_display);
        return false;
    }

    struct pollfd fds[2];
    int count = 1;
    fds[0] = { wl_display_get_fd(m_display), static_cast<short>(POLLIN | (m_need_flush ? POLLOUT : 0)), 0 };
    if (auth_fd.has_value()) {
        fds[1] = { *auth_fd, POLLIN, 0 };
        count = 2;
    }

    int const ready = poll(fds, count, timeout_ms);
    if (ready < 0) {
        wl_display_cancel_read(m_display);
        return errno == EINTR;
    }

    if (ready > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
        if (wl_display_read_events(m_display) < 0)
            return false;
    } else {
        wl_display_cancel_read(m_display);
    }

    if (ready > 0 && (fds[0].revents & POLLOUT)) {
        if (!flush_display())
            return false;
    }

    if (wl_display_dispatch_pending(m_display) < 0)
        return false;

    if (ready > 0 && count > 1 && (fds[1].revents & POLLIN))
        auth_ready = true;

    return true;
}

void WaylandSession::mark_all_dirty(bool full_redraw)
{
    for (auto* output : m_outputs) {
        output->panel_dirty = true;
        if (full_redraw)
            output->full_dirty = true;
    }
}

WaylandSession::Output* WaylandSession::effective_active_output() const
{
    if (m_active_output)
        return m_active_output;
    if (!m_outputs.is_empty())
        return m_outputs.first();
    return nullptr;
}

void WaylandSession::render_dirty(UiState const& state)
{
    bool any_committed = false;
    for (auto* output : m_outputs) {
        if (!output->configured || output->logical_size.is_empty())
            continue;
        if (!output->full_dirty && !output->panel_dirty)
            continue;
        render_output(*output, state);
        any_committed = true;
    }
    if (any_committed)
        (void)flush_display();
}

bool WaylandSession::ensure_slot(ShmSlot& slot, Gfx::IntSize pixel_size)
{
    if (slot.buffer && slot.pixel_size == pixel_size)
        return true;

    destroy_slot(slot);
    int const stride = pixel_size.width() * 4;
    size_t const bytes = static_cast<size_t>(stride) * pixel_size.height();
    if (bytes == 0)
        return false;

    int fd = memfd_create("sde-lock-shm", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        close(fd);
        return false;
    }

    void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }

    auto* pool = wl_shm_create_pool(m_shm, fd, static_cast<int32_t>(bytes));
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, pixel_size.width(), pixel_size.height(), stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buffer) {
        munmap(data, bytes);
        return false;
    }

    slot.buffer = buffer;
    slot.data = data;
    slot.size = bytes;
    slot.pixel_size = pixel_size;
    slot.busy = false;
    wl_buffer_add_listener(buffer, &s_buffer_listener, &slot);
    return true;
}

void WaylandSession::ensure_cursor(int scale)
{
    int const clamped = max(1, scale);
    if (!m_compositor || !m_shm)
        return;
    if (m_cursor_surface && m_cursor_scale == clamped && m_cursor_slot.buffer)
        return;

    auto bitmap_or_error = LockRenderer::make_cursor_bitmap(clamped);
    if (bitmap_or_error.is_error())
        return;
    auto bitmap = bitmap_or_error.release_value();

    if (!ensure_slot(m_cursor_slot, bitmap->size()))
        return;

    int const stride = bitmap->width() * 4;
    for (int y = 0; y < bitmap->height(); ++y)
        memcpy(static_cast<u8*>(m_cursor_slot.data) + static_cast<size_t>(y) * stride, bitmap->scanline_u8(y), stride);

    if (!m_cursor_surface)
        m_cursor_surface = wl_compositor_create_surface(m_compositor);

    wl_surface_set_buffer_scale(m_cursor_surface, clamped);
    wl_surface_attach(m_cursor_surface, m_cursor_slot.buffer, 0, 0);
    wl_surface_damage_buffer(m_cursor_surface, 0, 0, bitmap->width(), bitmap->height());
    wl_surface_commit(m_cursor_surface);
    m_cursor_scale = clamped;
}

void WaylandSession::render_output(Output& output, UiState const& state)
{
    Gfx::IntSize const pixel_size {
        output.logical_size.width() * output.scale,
        output.logical_size.height() * output.scale,
    };

    if (!output.framebuffer || output.framebuffer->size() != pixel_size) {
        auto fb = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, pixel_size);
        if (fb.is_error())
            return;
        output.framebuffer = fb.release_value();
        output.cached_background = nullptr;
        output.full_dirty = true;
    }

    if (!output.cached_background || output.cached_background->size() != pixel_size) {
        if (auto bg = m_renderer.render_background(pixel_size); !bg.is_error())
            output.cached_background = bg.release_value();
        output.full_dirty = true;
    }

    bool const show_panel = (&output == effective_active_output());
    Optional<Gfx::IntRect> dirty_rect;
    Gfx::IntRect damage = output.framebuffer->rect();

    if (!output.full_dirty && show_panel) {
        auto const layout = m_renderer.layout_for(output.logical_size, output.scale);
        damage = layout.panel.united(layout.shadow).intersected(output.framebuffer->rect());
        dirty_rect = damage;
    }

    m_renderer.render(*output.framebuffer, output.cached_background.ptr(), state, output.logical_size, output.scale, show_panel, dirty_rect);
    commit_output(output, damage);
    output.full_dirty = false;
    output.panel_dirty = false;
}

void WaylandSession::commit_output(Output& output, Gfx::IntRect damage_rect)
{
    auto& bitmap = *output.framebuffer;
    ShmSlot* slot = nullptr;
    if (!output.slots[0].busy)
        slot = &output.slots[0];
    else if (!output.slots[1].busy)
        slot = &output.slots[1];
    else
        slot = &output.slots[0];

    if (!ensure_slot(*slot, bitmap.size()))
        return;

    int const stride = bitmap.width() * 4;
    for (int y = 0; y < bitmap.height(); ++y)
        memcpy(static_cast<u8*>(slot->data) + static_cast<size_t>(y) * stride, bitmap.scanline_u8(y), stride);

    slot->busy = true;
    wl_surface_set_buffer_scale(output.surface, output.scale);
    wl_surface_attach(output.surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(output.surface, damage_rect.x(), damage_rect.y(), damage_rect.width(), damage_rect.height());
    wl_surface_commit(output.surface);
}

void WaylandSession::create_lock_surface(Output& output)
{
    if (!m_lock || !m_compositor || output.surface)
        return;

    output.surface = wl_compositor_create_surface(m_compositor);
    output.lock_surface = ext_session_lock_v1_get_lock_surface(m_lock, output.surface, output.output);
    ext_session_lock_surface_v1_add_listener(output.lock_surface, &s_lock_surface_listener, &output);
}

WaylandSession::Output* WaylandSession::output_for_surface(wl_surface* surface)
{
    for (auto* output : m_outputs) {
        if (output->surface == surface)
            return output;
    }
    return nullptr;
}

void WaylandSession::registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    auto& self = *static_cast<WaylandSession*>(data);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self.m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, min(version, 4u)));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        self.m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        self.m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 7u)));
        wl_seat_add_listener(self.m_seat, &s_seat_listener, &self);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        auto* output = new Output;
        output->session = &self;
        output->registry_name = name;
        output->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, min(version, 4u)));
        wl_output_add_listener(output->output, &s_output_listener, output);
        self.m_outputs.append(output);
        if (!self.m_active_output)
            self.m_active_output = output;
        if (self.m_lock)
            self.create_lock_surface(*output);
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        self.m_lock_manager = static_cast<ext_session_lock_manager_v1*>(wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
    }
}

void WaylandSession::registry_global_remove(void* data, wl_registry*, uint32_t name)
{
    auto& self = *static_cast<WaylandSession*>(data);
    for (size_t i = 0; i < self.m_outputs.size(); ++i) {
        auto* output = self.m_outputs[i];
        if (output->registry_name != name)
            continue;
        if (self.m_hover_output == output)
            self.m_hover_output = nullptr;
        if (self.m_active_output == output)
            self.m_active_output = nullptr;
        self.m_outputs.remove(i);
        self.destroy_output(output);
        if (!self.m_active_output && !self.m_outputs.is_empty()) {
            self.m_active_output = self.m_outputs.first();
            self.m_active_output->full_dirty = true;
        }
        return;
    }
}

void WaylandSession::seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities)
{
    auto& self = *static_cast<WaylandSession*>(data);

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !self.m_keyboard) {
        self.m_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(self.m_keyboard, &s_keyboard_listener, &self);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self.m_pointer) {
        self.m_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self.m_pointer, &s_pointer_listener, &self);
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && self.m_keyboard) {
        wl_keyboard_destroy(self.m_keyboard);
        self.m_keyboard = nullptr;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && self.m_pointer) {
        wl_pointer_destroy(self.m_pointer);
        self.m_pointer = nullptr;
    }
}

void WaylandSession::seat_name(void*, wl_seat*, char const*) { }

void WaylandSession::keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (self.on_keymap)
        self.on_keymap(format, fd, size);
    else if (fd >= 0)
        close(fd);
}

void WaylandSession::keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*)
{
    dbgln("sde-lock: keyboard ready");
}

void WaylandSession::keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { }

void WaylandSession::keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (self.on_key)
        self.on_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

void WaylandSession::keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (self.on_modifiers)
        self.on_modifiers(depressed, latched, locked, group);
}

void WaylandSession::keyboard_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (self.on_repeat_info)
        self.on_repeat_info(rate, delay);
}

void WaylandSession::pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
    auto& self = *static_cast<WaylandSession*>(data);
    auto* target = self.output_for_surface(surface);
    self.m_hover_output = target;
    self.m_pointer_position = { static_cast<int>(wl_fixed_to_double(x)), static_cast<int>(wl_fixed_to_double(y)) };

    if (target) {
        self.ensure_cursor(target->scale);
        if (self.m_cursor_surface)
            wl_pointer_set_cursor(pointer, serial, self.m_cursor_surface, 0, 0);

        // Move the unlock panel to whichever output the pointer enters.
        if (self.m_active_output != target) {
            if (self.m_active_output)
                self.m_active_output->full_dirty = true;
            self.m_active_output = target;
            target->full_dirty = true;
        }
    }
}

void WaylandSession::pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*)
{
    auto& self = *static_cast<WaylandSession*>(data);
    self.m_hover_output = nullptr;
    if (self.m_button_hovered) {
        self.m_button_hovered = false;
        if (self.on_button_hover_changed)
            self.on_button_hover_changed(false);
    }
}

void WaylandSession::pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto& self = *static_cast<WaylandSession*>(data);
    self.m_pointer_position = { static_cast<int>(wl_fixed_to_double(x)), static_cast<int>(wl_fixed_to_double(y)) };

    bool hovered = false;
    if (self.m_hover_output && self.m_hover_output->configured && self.m_hover_output == self.effective_active_output()) {
        auto const layout = self.m_renderer.layout_for(self.m_hover_output->logical_size, 1);
        hovered = layout.button.contains(self.m_pointer_position);
    }
    if (hovered != self.m_button_hovered) {
        self.m_button_hovered = hovered;
        if (self.on_button_hover_changed)
            self.on_button_hover_changed(hovered);
    }
}

void WaylandSession::pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    if (self.on_pointer_activity)
        self.on_pointer_activity();

    if (!self.m_hover_output || !self.m_hover_output->configured || self.m_hover_output != self.effective_active_output())
        return;

    auto const layout = self.m_renderer.layout_for(self.m_hover_output->logical_size, 1);
    if (layout.button.contains(self.m_pointer_position) && self.on_unlock_clicked)
        self.on_unlock_clicked();
}

void WaylandSession::pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { }
void WaylandSession::pointer_frame(void*, wl_pointer*) { }
void WaylandSession::pointer_axis_source(void*, wl_pointer*, uint32_t) { }
void WaylandSession::pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) { }
void WaylandSession::pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) { }
void WaylandSession::pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) { }
void WaylandSession::pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) { }
void WaylandSession::pointer_warp(void*, wl_pointer*, wl_fixed_t, wl_fixed_t) { }

void WaylandSession::lock_locked(void* data, ext_session_lock_v1*)
{
    auto& self = *static_cast<WaylandSession*>(data);
    self.m_locked = true;
    self.mark_all_dirty(true);
    if (self.on_locked)
        self.on_locked();
}

void WaylandSession::lock_finished(void* data, ext_session_lock_v1*)
{
    auto& self = *static_cast<WaylandSession*>(data);
    if (self.on_finished)
        self.on_finished();
}

void WaylandSession::lock_surface_configure(void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height)
{
    auto& output = *static_cast<Output*>(data);
    ext_session_lock_surface_v1_ack_configure(surface, serial);

    Gfx::IntSize new_size { static_cast<int>(width), static_cast<int>(height) };
    if (output.logical_size != new_size) {
        output.logical_size = new_size;
        output.cached_background = nullptr;
        output.framebuffer = nullptr;
    }
    output.configured = true;
    output.full_dirty = true;
    output.panel_dirty = true;
}

void WaylandSession::output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, char const*, char const*, int32_t) { }
void WaylandSession::output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) { }
void WaylandSession::output_done(void*, wl_output*) { }

void WaylandSession::output_scale(void* data, wl_output*, int32_t factor)
{
    auto& output = *static_cast<Output*>(data);
    if (factor <= 0 || output.scale == factor)
        return;
    output.scale = factor;
    output.cached_background = nullptr;
    output.framebuffer = nullptr;
    output.full_dirty = true;
    output.panel_dirty = true;
}

void WaylandSession::output_name(void*, wl_output*, char const*) { }
void WaylandSession::output_description(void*, wl_output*, char const*) { }

void WaylandSession::buffer_release(void* data, wl_buffer*)
{
    auto* slot = static_cast<ShmSlot*>(data);
    slot->busy = false;
}

}
