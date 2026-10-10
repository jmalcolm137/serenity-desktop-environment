/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * serenity-window-server: the Serenity Desktop Environment's Wayland compositor.
 *
 * Phase 0 skeleton. It uses wlroots for the display/input plumbing and
 * SerenityOS userland (AK, LibCore, ...) for everything we write, following
 * SerenityOS conventions. Rendering chrome with LibGfx/LibGPU comes next.
 */

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/WindowTheme.h>
#include <LibMain/Main.h>

#include <ctime>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
}

namespace {

enum class CursorMode {
    Passthrough,
    Move,
    Resize,
};

struct Server;
struct Output;
struct Toplevel;
struct Popup;
struct LayerSurface;
struct Keyboard;
struct Decoration;

static void toplevel_update_frame(Toplevel&);
static bool handle_frame_press(Server&, uint32_t button);
static void arrange_layers(Server&, wlr_output*);
static Output* output_for(Server&, wlr_output*);
static void output_manager_send_configuration(Server&);
static Gfx::Palette* g_palette { nullptr };

struct FrameIcons {
    RefPtr<Gfx::Bitmap> close;
    RefPtr<Gfx::Bitmap> minimize;
    RefPtr<Gfx::Bitmap> maximize;
    RefPtr<Gfx::Bitmap> restore;
};
static FrameIcons g_frame_icons;

static RefPtr<Gfx::Bitmap> load_icon(ByteString const& resource_root, StringView name, StringView fallback)
{
    auto path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, name);
    if (auto bitmap = Gfx::Bitmap::load_from_file(path); !bitmap.is_error())
        return bitmap.release_value();
    if (!fallback.is_empty()) {
        auto fallback_path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, fallback);
        if (auto bitmap = Gfx::Bitmap::load_from_file(fallback_path); !bitmap.is_error())
            return bitmap.release_value();
    }
    return nullptr;
}

// A wlr_buffer backed by a Gfx::Bitmap, so LibGfx-drawn chrome (window frames,
// overlays) can be presented through the wlroots scene graph.
struct BitmapBuffer {
    wlr_buffer base { };
    NonnullRefPtr<Gfx::Bitmap> bitmap;

    explicit BitmapBuffer(NonnullRefPtr<Gfx::Bitmap> bitmap_)
        : bitmap(move(bitmap_))
    {
    }
};

static void bitmap_buffer_destroy(wlr_buffer* buffer)
{
    auto* self = reinterpret_cast<BitmapBuffer*>(buffer);
    wlr_buffer_finish(buffer);
    delete self;
}

static bool bitmap_buffer_get_dmabuf(wlr_buffer*, wlr_dmabuf_attributes*) { return false; }
static bool bitmap_buffer_get_shm(wlr_buffer*, wlr_shm_attributes*) { return false; }

static bool bitmap_buffer_begin_data_ptr_access(wlr_buffer* buffer, uint32_t, void** data, uint32_t* format, size_t* stride)
{
    auto* self = reinterpret_cast<BitmapBuffer*>(buffer);
    *data = self->bitmap->scanline_u8(0);
    *format = DRM_FORMAT_ARGB8888;
    *stride = self->bitmap->pitch();
    return true;
}

static void bitmap_buffer_end_data_ptr_access(wlr_buffer*) { }

static constexpr struct wlr_buffer_impl BITMAP_BUFFER_IMPL = {
    .destroy = bitmap_buffer_destroy,
    .get_dmabuf = bitmap_buffer_get_dmabuf,
    .get_shm = bitmap_buffer_get_shm,
    .begin_data_ptr_access = bitmap_buffer_begin_data_ptr_access,
    .end_data_ptr_access = bitmap_buffer_end_data_ptr_access,
};

static wlr_buffer* bitmap_buffer_create(NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    auto* buffer = new BitmapBuffer(move(bitmap));
    wlr_buffer_init(&buffer->base, &BITMAP_BUFFER_IMPL, buffer->bitmap->width(), buffer->bitmap->height());
    return &buffer->base;
}

struct Server {
    wl_display* display { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* output_layout { nullptr };
    wlr_xdg_shell* xdg_shell { nullptr };
    wlr_layer_shell_v1* layer_shell { nullptr };
    wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager { nullptr };
    wlr_output_manager_v1* output_manager { nullptr };
    wlr_cursor* cursor { nullptr };
    wlr_xcursor_manager* cursor_manager { nullptr };
    wlr_seat* seat { nullptr };
    wlr_output* primary_output { nullptr };

    wl_listener new_output { };
    wl_listener new_xdg_toplevel { };
    wl_listener new_xdg_popup { };
    wl_listener new_layer_surface { };
    wl_listener new_toplevel_decoration { };
    wl_listener output_manager_apply { };
    wl_listener output_manager_test { };
    wl_listener new_input { };
    wl_listener cursor_motion { };
    wl_listener cursor_motion_absolute { };
    wl_listener cursor_button { };
    wl_listener cursor_axis { };
    wl_listener cursor_frame { };
    wl_listener seat_request_cursor { };
    wl_listener seat_pointer_focus_change { };
    wl_listener seat_request_set_selection { };

    Vector<NonnullOwnPtr<Output>> outputs;
    Vector<NonnullOwnPtr<Toplevel>> toplevels;
    Vector<NonnullOwnPtr<Popup>> popups;
    Vector<NonnullOwnPtr<LayerSurface>> layer_surfaces;
    Vector<NonnullOwnPtr<Keyboard>> keyboards;
    Vector<NonnullOwnPtr<Decoration>> decorations;

    CursorMode cursor_mode { CursorMode::Passthrough };
    Toplevel* grabbed_toplevel { nullptr };
    double grab_x { 0 };
    double grab_y { 0 };
    wlr_box grab_box { };
    uint32_t resize_edges { 0 };
    bool frame_grab { false };
};

struct Output {
    Server* server { nullptr };
    wlr_output* output { nullptr };
    wlr_box usable_area { };
    int panel_reserve_bottom { 0 };
    wl_listener frame { };
    wl_listener request_state { };
    wl_listener destroy { };
};

struct Toplevel {
    Server* server { nullptr };
    wlr_xdg_toplevel* xdg_toplevel { nullptr };
    wlr_scene_tree* scene_tree { nullptr };
    wlr_scene_tree* content_tree { nullptr };
    wlr_scene_buffer* frame_buffer { nullptr };
    int frame_left { 0 };
    int frame_top { 0 };
    int frame_right { 0 };
    int frame_bottom { 0 };
    Gfx::IntSize frame_size;
    Vector<Gfx::IntRect> button_rects;
    Gfx::IntSize restore_size;
    bool frame_active { true };
    bool maximized { false };
    bool minimized { false };
    bool is_panel { false };
    wlr_foreign_toplevel_handle_v1* foreign_handle { nullptr };
    wl_listener map { };
    wl_listener unmap { };
    wl_listener commit { };
    wl_listener destroy { };
    wl_listener request_move { };
    wl_listener request_resize { };
    wl_listener request_maximize { };
    wl_listener request_fullscreen { };
    wl_listener set_title { };
    wl_listener set_app_id { };
    wl_listener foreign_request_activate { };
    wl_listener foreign_request_close { };
};

struct Popup {
    Server* server { nullptr };
    wlr_xdg_popup* xdg_popup { nullptr };
    wl_listener commit { };
    wl_listener destroy { };
};

struct LayerSurface {
    Server* server { nullptr };
    wlr_layer_surface_v1* layer_surface { nullptr };
    wlr_scene_layer_surface_v1* scene { nullptr };
    wlr_output* output { nullptr };
    wl_listener map { };
    wl_listener unmap { };
    wl_listener commit { };
    wl_listener destroy { };
};

struct Keyboard {
    Server* server { nullptr };
    wlr_keyboard* keyboard { nullptr };
    wl_listener modifiers { };
    wl_listener key { };
    wl_listener destroy { };
};

struct Decoration {
    Server* server { nullptr };
    wlr_xdg_toplevel_decoration_v1* decoration { nullptr };
    wl_listener request_mode { };
    wl_listener destroy { };
};

template<typename T>
static void remove_owned(Vector<NonnullOwnPtr<T>>& list, T* object)
{
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].ptr() == object) {
            list.remove(index);
            return;
        }
    }
}

static Toplevel* toplevel_from_handle(wlr_foreign_toplevel_handle_v1* handle)
{
    return static_cast<Toplevel*>(handle->data);
}

static void focus_toplevel(Server& server, Toplevel* toplevel)
{
    if (!toplevel)
        return;

    wlr_surface* previous_surface = server.seat->keyboard_state.focused_surface;
    wlr_surface* surface = toplevel->xdg_toplevel->base->surface;
    if (previous_surface == surface)
        return;

    if (previous_surface) {
        if (auto* previous = wlr_xdg_toplevel_try_from_wlr_surface(previous_surface)) {
            wlr_xdg_toplevel_set_activated(previous, false);
            if (auto* content_tree = static_cast<wlr_scene_tree*>(previous->base->data)) {
                if (auto* frame_tree = content_tree->node.parent) {
                    if (auto* previous_toplevel = static_cast<Toplevel*>(frame_tree->node.data)) {
                        previous_toplevel->frame_active = false;
                        toplevel_update_frame(*previous_toplevel);
                    }
                }
            }
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    // Keep panels above normal windows.
    for (auto& candidate : server.toplevels) {
        if (candidate->is_panel && !candidate->minimized)
            wlr_scene_node_raise_to_top(&candidate->scene_tree->node);
    }

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    toplevel->frame_active = true;
    toplevel_update_frame(*toplevel);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_handle, true);

    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

// --- keyboard -------------------------------------------------------------

static void keyboard_modifiers(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard->modifiers);
}

static bool handle_keybinding(Server& server, xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server.display);
        return true;
    default:
        return false;
    }
}

static void keyboard_key(wl_listener* listener, void* data)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, key);
    auto& server = *keyboard->server;
    auto* event = static_cast<wlr_keyboard_key_event*>(data);

    uint32_t keycode = event->keycode + 8;
    xkb_keysym_t const* syms = nullptr;
    int syms_count = xkb_state_key_get_syms(keyboard->keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < syms_count; ++i)
            handled |= handle_keybinding(server, syms[i]);
    }

    if (!handled) {
        wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_destroy(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    remove_owned(keyboard->server->keyboards, keyboard);
}

// --- cursor / pointer -----------------------------------------------------

static Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy)
{
    wlr_scene_node* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    auto* scene_buffer = wlr_scene_buffer_from_node(node);
    auto* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface)
        return nullptr;

    *surface = scene_surface->surface;
    wlr_scene_tree* tree = node->parent;
    while (tree && !tree->node.data)
        tree = tree->node.parent;
    return tree ? static_cast<Toplevel*>(tree->node.data) : nullptr;
}

static void reset_cursor_mode(Server& server)
{
    server.cursor_mode = CursorMode::Passthrough;
    server.grabbed_toplevel = nullptr;
}

static void process_cursor_move(Server& server)
{
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server.cursor->x - server.grab_x, server.cursor->y - server.grab_y);
}

static void process_cursor_resize(Server& server)
{
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;

    double border_x = server.cursor->x - server.grab_x;
    double border_y = server.cursor->y - server.grab_y;
    int new_left = server.grab_box.x;
    int new_right = server.grab_box.x + server.grab_box.width;
    int new_top = server.grab_box.y;
    int new_bottom = server.grab_box.y + server.grab_box.height;

    if (server.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom)
            new_top = new_bottom - 1;
    } else if (server.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top)
            new_bottom = new_top + 1;
    }
    if (server.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right)
            new_left = new_right - 1;
    } else if (server.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left)
            new_right = new_left + 1;
    }

    auto* geometry = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geometry->x, new_top - geometry->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
}

static void process_cursor_motion(Server& server, uint32_t time)
{
    if (server.cursor_mode == CursorMode::Move) {
        process_cursor_move(server);
        return;
    }
    if (server.cursor_mode == CursorMode::Resize) {
        process_cursor_resize(server);
        return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    auto* toplevel = toplevel_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);
    if (!toplevel)
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");

    if (surface) {
        wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(server.seat);
    }
}

static void cursor_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion);
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(*server, event->time_msec);
}

static void cursor_motion_absolute(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion_absolute);
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(*server, event->time_msec);
}

static void cursor_button(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_button);
    auto* event = static_cast<wlr_pointer_button_event*>(data);

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (handle_frame_press(*server, event->button)) {
            server->frame_grab = true;
            return;
        }
    } else if (server->frame_grab) {
        server->frame_grab = false;
        reset_cursor_mode(*server);
        return;
    }

    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_cursor_mode(*server);
    } else {
        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        focus_toplevel(*server, toplevel_at(*server, server->cursor->x, server->cursor->y, &surface, &sx, &sy));
    }
}

static void cursor_axis(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_axis);
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_cursor);
    auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    if (server->seat->pointer_state.focused_client == event->seat_client)
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_pointer_focus_change(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_pointer_focus_change);
    auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
    if (!event->new_surface)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
}

static void seat_request_set_selection(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_set_selection);
    auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void server_new_input(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_input);
    auto* device = static_cast<wlr_input_device*>(data);

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        auto keyboard = make<Keyboard>();
        keyboard->server = server;
        keyboard->keyboard = wlr_keyboard_from_input_device(device);

        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(keyboard->keyboard, 25, 600);

        keyboard->modifiers.notify = keyboard_modifiers;
        wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
        keyboard->key.notify = keyboard_key;
        wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
        keyboard->destroy.notify = keyboard_destroy;
        wl_signal_add(&device->events.destroy, &keyboard->destroy);

        wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
        server->keyboards.append(move(keyboard));
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!server->keyboards.is_empty())
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, capabilities);
}

// --- outputs --------------------------------------------------------------

static void output_frame(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, frame);
    auto* scene_output = wlr_scene_get_scene_output(output->server->scene, output->output);
    if (!scene_output)
        return;
    wlr_scene_output_commit(scene_output, nullptr);
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(wl_listener* listener, void* data)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, request_state);
    auto* event = static_cast<wlr_output_event_request_state*>(data);
    wlr_output_commit_state(output->output, event->state);
}

static void output_destroy(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    remove_owned(output->server->outputs, output);
}

static void server_new_output(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_output);
    auto* wlr_output = static_cast<struct wlr_output*>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (auto* mode = wlr_output_preferred_mode(wlr_output))
        wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    auto output = make<Output>();
    output->server = server;
    output->output = wlr_output;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    auto* layout_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    auto* scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

    if (!server->primary_output)
        server->primary_output = wlr_output;

    dbgln("serenity-window-server: output '{}' {}x{}", wlr_output->name ? wlr_output->name : "?", wlr_output->width, wlr_output->height);
    server->outputs.append(move(output));
    arrange_layers(*server, wlr_output);
    output_manager_send_configuration(*server);
}

// --- xdg toplevels / popups ----------------------------------------------

static void toplevel_update_foreign(Toplevel& toplevel)
{
    if (!toplevel.foreign_handle)
        return;
    wlr_foreign_toplevel_handle_v1_set_title(toplevel.foreign_handle, toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(toplevel.foreign_handle, toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "");
}

static void toplevel_update_frame(Toplevel& toplevel)
{
    if (!g_palette)
        return;

    auto* xdg_toplevel = toplevel.xdg_toplevel;
    int content_width = xdg_toplevel->base->geometry.width;
    int content_height = xdg_toplevel->base->geometry.height;
    if (content_width <= 0 || content_height <= 0) {
        content_width = xdg_toplevel->base->surface->current.width;
        content_height = xdg_toplevel->base->surface->current.height;
    }
    if (content_width <= 0 || content_height <= 0)
        return;

    bool const is_panel = xdg_toplevel->app_id && strcmp(xdg_toplevel->app_id, "serenity-taskbar") == 0;
    toplevel.is_panel = is_panel;
    if (is_panel) {
        // The Taskbar is a bottom panel: no frame, full output width, and it
        // reserves a strip at the bottom of the usable area.
        if (auto* output = output_for(*toplevel.server, toplevel.server->primary_output)) {
            wlr_box box { };
            wlr_output_layout_get_box(toplevel.server->output_layout, output->output, &box);
            if (content_width != box.width) {
                wlr_xdg_toplevel_set_size(xdg_toplevel, box.width, content_height);
                return;
            }
            if (toplevel.frame_buffer) {
                wlr_scene_node_destroy(&toplevel.frame_buffer->node);
                toplevel.frame_buffer = nullptr;
            }
            wlr_scene_node_set_position(&toplevel.content_tree->node, 0, 0);
            wlr_scene_node_set_position(&toplevel.scene_tree->node, box.x, box.y + box.height - content_height);
            output->panel_reserve_bottom = content_height;
            arrange_layers(*toplevel.server, output->output);
        }
        return;
    }

    auto& theme = g_palette->window_theme();
    Gfx::IntRect content_rect { 0, 0, content_width, content_height };
    auto frame_rect = theme.frame_rect_for_window(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 0);
    auto frame_size = frame_rect.size();
    if (frame_size.is_empty())
        return;

    toplevel.frame_left = -frame_rect.x();
    toplevel.frame_top = -frame_rect.y();
    toplevel.frame_right = frame_size.width() - content_width - toplevel.frame_left;
    toplevel.frame_bottom = frame_size.height() - content_height - toplevel.frame_top;
    toplevel.frame_size = frame_size;

    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, frame_size).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    painter.fill_rect(Gfx::IntRect { { }, frame_size }, g_palette->window());

    static auto const icon = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }).release_value_but_fixme_should_propagate_errors();
    auto buttons = theme.layout_buttons(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 3, false);
    Gfx::IntRect leftmost_button = buttons.is_empty() ? Gfx::IntRect { } : buttons.last();
    auto state = toplevel.frame_active ? Gfx::WindowTheme::WindowState::Active : Gfx::WindowTheme::WindowState::Inactive;
    StringView title;
    if (xdg_toplevel->title)
        title = StringView { xdg_toplevel->title, strlen(xdg_toplevel->title) };
    theme.paint_normal_frame(painter, state, Gfx::WindowTheme::WindowMode::Other, content_rect, title, *icon, *g_palette, leftmost_button, 0, false);

    toplevel.button_rects.clear_with_capacity();
    for (size_t index = 0; index < buttons.size(); ++index) {
        auto button_rect = buttons[index].translated(toplevel.frame_left, toplevel.frame_top);
        toplevel.button_rects.append(button_rect);
        if (!g_palette->title_buttons_icon_only())
            Gfx::StylePainter::paint_button(painter, button_rect, *g_palette, Gfx::ButtonStyle::Normal, false, false);
        RefPtr<Gfx::Bitmap> button_icon;
        if (index == 0)
            button_icon = g_frame_icons.close;
        else if (index == 1)
            button_icon = toplevel.maximized ? g_frame_icons.restore : g_frame_icons.maximize;
        else if (index == 2)
            button_icon = g_frame_icons.minimize;
        if (button_icon)
            painter.blit(button_rect.center().translated(-(button_icon->width() / 2), -(button_icon->height() / 2)), *button_icon, button_icon->rect());
    }

    wlr_scene_node_set_position(&toplevel.content_tree->node, toplevel.frame_left, toplevel.frame_top);

    if (auto const* dump_path = getenv("SERENITY_WINDOW_SERVER_DUMP_FRAME"); dump_path && *dump_path) {
        if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
            if (auto file = Core::File::open(StringView { dump_path, strlen(dump_path) }, Core::File::OpenMode::Write); !file.is_error()) {
                if (auto write_result = file.value()->write_until_depleted(encoded.value()); write_result.is_error())
                    warnln("serenity-window-server: could not write the frame dump: {}", write_result.error());
            }
        }
    }

    auto* buffer = bitmap_buffer_create(move(bitmap));
    if (!toplevel.frame_buffer) {
        toplevel.frame_buffer = wlr_scene_buffer_create(toplevel.scene_tree, buffer);
        wlr_scene_node_lower_to_bottom(&toplevel.frame_buffer->node);
        wlr_scene_node_set_position(&toplevel.frame_buffer->node, 0, 0);
    } else {
        wlr_scene_buffer_set_buffer(toplevel.frame_buffer, buffer);
    }
    wlr_buffer_drop(buffer);
}

static void toplevel_map(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, map);
    toplevel_update_foreign(*toplevel);
    dbgln("serenity-window-server: toplevel mapped title='{}' app_id='{}'",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
        toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");
    toplevel->frame_active = true;
    toplevel_update_frame(*toplevel);
    if (toplevel->foreign_handle && toplevel->server->primary_output)
        wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign_handle, toplevel->server->primary_output);
    focus_toplevel(*toplevel->server, toplevel);
}

static void toplevel_unmap(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, unmap);
    if (toplevel == toplevel->server->grabbed_toplevel)
        reset_cursor_mode(*toplevel->server);

    if (toplevel->is_panel) {
        if (auto* output = output_for(*toplevel->server, toplevel->server->primary_output)) {
            output->panel_reserve_bottom = 0;
            arrange_layers(*toplevel->server, output->output);
        }
    }
}

static void toplevel_commit(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit)
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    toplevel_update_foreign(*toplevel);
    toplevel_update_frame(*toplevel);
}

static void toplevel_destroy(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_app_id.link);
    wl_list_remove(&toplevel->foreign_request_activate.link);
    wl_list_remove(&toplevel->foreign_request_close.link);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);
    if (toplevel->is_panel) {
        if (auto* output = output_for(*toplevel->server, toplevel->server->primary_output)) {
            output->panel_reserve_bottom = 0;
            arrange_layers(*toplevel->server, output->output);
        }
    }
    remove_owned(server.toplevels, toplevel);
}

static void toplevel_begin_interactive(Toplevel& toplevel, CursorMode mode, uint32_t edges)
{
    auto& server = *toplevel.server;
    server.grabbed_toplevel = &toplevel;
    server.cursor_mode = mode;
    if (mode == CursorMode::Move) {
        server.grab_x = server.cursor->x - toplevel.scene_tree->node.x;
        server.grab_y = server.cursor->y - toplevel.scene_tree->node.y;
    } else {
        auto* geometry = &toplevel.xdg_toplevel->base->geometry;
        double border_x = (toplevel.scene_tree->node.x + geometry->x) + ((edges & WLR_EDGE_RIGHT) ? geometry->width : 0);
        double border_y = (toplevel.scene_tree->node.y + geometry->y) + ((edges & WLR_EDGE_BOTTOM) ? geometry->height : 0);
        server.grab_x = server.cursor->x - border_x;
        server.grab_y = server.cursor->y - border_y;
        server.grab_box = *geometry;
        server.grab_box.x += toplevel.scene_tree->node.x;
        server.grab_box.y += toplevel.scene_tree->node.y;
        server.resize_edges = edges;
    }
}

static void toplevel_close(Toplevel& toplevel)
{
    wlr_xdg_toplevel_send_close(toplevel.xdg_toplevel);
}

static void toplevel_toggle_maximized(Toplevel& toplevel)
{
    auto& server = *toplevel.server;
    if (!toplevel.maximized) {
        toplevel.restore_size = { toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
        if (auto* output_state = output_for(server, server.primary_output); output_state && !wlr_box_empty(&output_state->usable_area))
            box = output_state->usable_area;
        toplevel.maximized = true;
        wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, true);
        if (server.primary_output)
            wlr_scene_node_set_position(&toplevel.scene_tree->node, box.x, box.y);
        wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, box.width - toplevel.frame_left - toplevel.frame_right, box.height - toplevel.frame_top - toplevel.frame_bottom);
    } else {
        toplevel.maximized = false;
        wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
        wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
    }
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, toplevel.maximized);
}

static void toplevel_toggle_minimized(Toplevel& toplevel)
{
    toplevel.minimized = !toplevel.minimized;
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node, !toplevel.minimized);
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel.foreign_handle, toplevel.minimized);
}

static bool handle_frame_press(Server& server, uint32_t button)
{
    if (button != BTN_LEFT)
        return false;

    int cursor_x = (int)server.cursor->x;
    int cursor_y = (int)server.cursor->y;
    for (size_t index = server.toplevels.size(); index-- > 0;) {
        auto& toplevel = *server.toplevels[index];
        if (toplevel.minimized)
            continue;

        auto* node = &toplevel.scene_tree->node;
        Gfx::IntRect frame_rect { node->x, node->y, toplevel.frame_size.width(), toplevel.frame_size.height() };
        if (frame_rect.is_empty() || !frame_rect.contains({ cursor_x, cursor_y }))
            continue;

        Gfx::IntPoint local { cursor_x - node->x, cursor_y - node->y };
        for (size_t button_index = 0; button_index < toplevel.button_rects.size(); ++button_index) {
            if (!toplevel.button_rects[button_index].contains(local))
                continue;
            if (button_index == 0)
                toplevel_close(toplevel);
            else if (button_index == 1)
                toplevel_toggle_maximized(toplevel);
            else if (button_index == 2)
                toplevel_toggle_minimized(toplevel);
            return true;
        }

        if (local.y() < toplevel.frame_top) {
            toplevel_begin_interactive(toplevel, CursorMode::Move, 0);
            return true;
        }

        constexpr int resize_border = 4;
        uint32_t edges = 0;
        if (local.x() < resize_border)
            edges |= WLR_EDGE_LEFT;
        if (local.x() >= toplevel.frame_size.width() - resize_border)
            edges |= WLR_EDGE_RIGHT;
        if (local.y() >= toplevel.frame_size.height() - resize_border)
            edges |= WLR_EDGE_BOTTOM;
        if (edges) {
            toplevel_begin_interactive(toplevel, CursorMode::Resize, edges);
            return true;
        }
        return false;
    }
    return false;
}

static void toplevel_request_move(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_move);
    toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
}

static void toplevel_request_resize(wl_listener* listener, void* data)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_resize);
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    toplevel_begin_interactive(*toplevel, CursorMode::Resize, event->edges);
}

static void toplevel_request_maximize(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_request_fullscreen(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_set_title(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_title);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_set_app_id(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_app_id);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_foreign_request_activate(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_activate);
    focus_toplevel(*toplevel->server, toplevel);
}

static void toplevel_foreign_request_close(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_close);
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void server_new_xdg_toplevel(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_toplevel);
    auto* xdg_toplevel = static_cast<wlr_xdg_toplevel*>(data);

    auto toplevel = make<Toplevel>();
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    toplevel->scene_tree->node.data = toplevel.ptr();
    toplevel->content_tree = wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    xdg_toplevel->base->data = toplevel->content_tree;

    if (server->foreign_toplevel_manager) {
        toplevel->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
        toplevel->foreign_handle->data = toplevel.ptr();
        toplevel->foreign_request_activate.notify = toplevel_foreign_request_activate;
        wl_signal_add(&toplevel->foreign_handle->events.request_activate, &toplevel->foreign_request_activate);
        toplevel->foreign_request_close.notify = toplevel_foreign_request_close;
        wl_signal_add(&toplevel->foreign_handle->events.request_close, &toplevel->foreign_request_close);
    }

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->set_app_id.notify = toplevel_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

    server->toplevels.append(move(toplevel));
}

static void popup_commit(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit)
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    remove_owned(popup->server->popups, popup);
}

static void server_new_xdg_popup(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_popup);
    auto* xdg_popup = static_cast<wlr_xdg_popup*>(data);

    auto popup = make<Popup>();
    popup->server = server;
    popup->xdg_popup = xdg_popup;

    auto* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (!parent)
        return;
    auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

    server->popups.append(move(popup));
}

// --- layer shell ----------------------------------------------------------

static Output* output_for(Server& server, wlr_output* output)
{
    for (auto& candidate : server.outputs) {
        if (candidate->output == output)
            return candidate.ptr();
    }
    return nullptr;
}

static void arrange_layers(Server& server, wlr_output* output)
{
    if (!output)
        return;

    wlr_box full_area { };
    wlr_output_layout_get_box(server.output_layout, output, &full_area);
    wlr_box usable_area = full_area;

    auto* output_state = output_for(server, output);
    if (output_state && output_state->panel_reserve_bottom > 0)
        usable_area.height = max(0, usable_area.height - output_state->panel_reserve_bottom);

    // Configure layer surfaces from the bottom-most layer up, so exclusive
    // zones from lower layers shrink the area available to higher ones.
    static constexpr zwlr_layer_shell_v1_layer layer_order[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    };

    for (auto layer : layer_order) {
        for (auto& layer_surface : server.layer_surfaces) {
            auto* surface = layer_surface->layer_surface;
            if (surface->output != output)
                continue;
            if (surface->current.layer != layer)
                continue;
            wlr_scene_layer_surface_v1_configure(layer_surface->scene, &full_area, &usable_area);
        }
    }

    if (output_state) {
        output_state->usable_area = usable_area;
        dbgln("serenity-window-server: usable area for '{}': {}x{} at {},{}", output->name ? output->name : "?",
            usable_area.width, usable_area.height, usable_area.x, usable_area.y);
    }
}

static void layer_surface_commit(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, commit);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_map(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, map);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, true);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_unmap(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, unmap);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_destroy(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, destroy);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->destroy.link);
    auto* server = layer->server;
    auto* output = layer->output;
    remove_owned(server->layer_surfaces, layer);
    arrange_layers(*server, output);
}

static void server_new_layer_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_layer_surface);
    auto* layer_surface = static_cast<wlr_layer_surface_v1*>(data);

    if (!layer_surface->output)
        layer_surface->output = server->primary_output;
    if (!layer_surface->output) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    auto layer = make<LayerSurface>();
    layer->server = server;
    layer->layer_surface = layer_surface;
    layer->output = layer_surface->output;
    dbgln("serenity-window-server: layer surface namespace='{}'", layer_surface->layer_namespace ? layer_surface->layer_namespace : "");
    layer->scene = wlr_scene_layer_surface_v1_create(&server->scene->tree, layer_surface);
    layer->scene->tree->node.data = layer.ptr();
    layer_surface->data = layer.ptr();

    layer->map.notify = layer_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &layer->map);
    layer->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
    layer->commit.notify = layer_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
    layer->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

    server->layer_surfaces.append(move(layer));
}

// --- xdg-decoration -------------------------------------------------------

static void decoration_request_mode(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, request_mode);
    auto requested = decoration->decoration->requested_mode;
    // Strongly prefer server-side decorations, but honour an explicit CSD request.
    if (requested == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration, requested);
    else
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_destroy(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, destroy);
    wl_list_remove(&decoration->request_mode.link);
    wl_list_remove(&decoration->destroy.link);
    remove_owned(decoration->server->decorations, decoration);
}

static void server_new_toplevel_decoration(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_toplevel_decoration);
    auto* wlr_decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);

    auto decoration = make<Decoration>();
    decoration->server = server;
    decoration->decoration = wlr_decoration;
    decoration->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wlr_decoration->events.request_mode, &decoration->request_mode);
    decoration->destroy.notify = decoration_destroy;
    wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);

    wlr_xdg_toplevel_decoration_v1_set_mode(wlr_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    server->decorations.append(move(decoration));
}

// --- output management ----------------------------------------------------

static void output_manager_send_configuration(Server& server)
{
    if (!server.output_manager)
        return;

    auto* config = wlr_output_configuration_v1_create();
    for (auto& output : server.outputs) {
        auto* head = wlr_output_configuration_head_v1_create(config, output->output);
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output->output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }
    wlr_output_manager_v1_set_configuration(server.output_manager, config);
}

static void output_manager_apply_or_test(Server& server, wlr_output_configuration_v1* config, bool test)
{
    size_t states_len = 0;
    auto* states = wlr_output_configuration_v1_build_state(config, &states_len);
    bool ok = states != nullptr
        && (test ? wlr_backend_test(server.backend, states, states_len)
                 : wlr_backend_commit(server.backend, states, states_len));
    free(states);

    if (!ok) {
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
    }

    if (!test) {
        wlr_output_configuration_head_v1* head = nullptr;
        wl_list_for_each(head, &config->heads, link)
        {
            if (head->state.enabled)
                wlr_output_layout_add(server.output_layout, head->state.output, head->state.x, head->state.y);
        }
    }

    wlr_output_configuration_v1_send_succeeded(config);
    wlr_output_configuration_v1_destroy(config);

    if (!test)
        output_manager_send_configuration(server);
}

static void output_manager_apply(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_apply);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), false);
}

static void output_manager_test(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_test);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), true);
}

// --- setup ----------------------------------------------------------------

static ErrorOr<void> initialize(Server& server)
{
    server.display = wl_display_create();
    if (!server.display)
        return Error::from_string_literal("failed to create wl_display");

    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), nullptr);
    if (!server.backend)
        return Error::from_string_literal("failed to create wlr_backend");

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer)
        return Error::from_string_literal("failed to create wlr_renderer");
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator)
        return Error::from_string_literal("failed to create wlr_allocator");

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);

    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    if (auto* decoration_manager = wlr_xdg_decoration_manager_v1_create(server.display)) {
        server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
        wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_toplevel_decoration);
    }

    server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.display);

    server.output_manager = wlr_output_manager_v1_create(server.display);
    server.output_manager_apply.notify = output_manager_apply;
    wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
    server.output_manager_test.notify = output_manager_test;
    wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_manager = wlr_xcursor_manager_create(nullptr, 24);

    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.seat_request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_cursor);
    server.seat_pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.seat_pointer_focus_change);
    server.seat_request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);

    return { };
}

static ByteString add_socket(Server& server, ByteString const& requested)
{
    if (!requested.is_empty()) {
        if (wl_display_add_socket(server.display, requested.characters()) == 0)
            return requested;
        warnln("serenity-window-server: could not add socket '{}'", requested);
    }
    auto const* socket = wl_display_add_socket_auto(server.display);
    return socket ? ByteString(socket) : ByteString { };
}

static void spawn_startup_command(StringView command)
{
    if (command.is_empty())
        return;
    auto command_string = command.to_byte_string();
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", command_string.characters(), static_cast<char*>(nullptr));
        _exit(127);
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

    g_frame_icons.close = load_icon(resource_root, "window-close"sv, "window-close"sv);
    g_frame_icons.minimize = load_icon(resource_root, "window-minimize"sv, "downward-triangle"sv);
    g_frame_icons.maximize = load_icon(resource_root, "window-maximize"sv, "upward-triangle"sv);
    g_frame_icons.restore = load_icon(resource_root, "window-restore"sv, "upward-triangle"sv);

    dbgln("serenity-window-server: loaded the Serenity theme from {}", resource_root);
    return { };
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    wlr_log_init(WLR_ERROR, nullptr);

    StringView startup_command;
    StringView socket_name;
    Core::ArgsParser args_parser;
    args_parser.add_option(startup_command, "Command to run once the compositor is up", nullptr, 's', "command");
    args_parser.add_option(socket_name, "Wayland socket name", nullptr, 'n', "name");
    args_parser.parse(arguments);

    TRY(initialize_gfx());

    auto server = make<Server>();
    if (auto result = initialize(*server); result.is_error()) {
        warnln("serenity-window-server: {}", result.error());
        return 1;
    }

    auto socket = add_socket(*server, socket_name.to_byte_string());
    if (socket.is_empty()) {
        warnln("serenity-window-server: failed to create a Wayland socket");
        return 1;
    }

    if (!wlr_backend_start(server->backend)) {
        warnln("serenity-window-server: failed to start the backend");
        wl_display_destroy(server->display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket.characters(), true);
    dbgln("serenity-window-server: running on WAYLAND_DISPLAY={}", socket);
    spawn_startup_command(startup_command);

    wl_display_run(server->display);
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);
    return 0;
}
