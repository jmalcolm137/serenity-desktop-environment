/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 ScreenCast backend for the SDE portal service.
//
// Captures an output/window with the *standard* ext-image-copy-capture-v1
// protocol and publishes the frames as a PipeWire video stream, which the
// frontend hands to the requesting client. Buffers are DMA-BUF (zero-copy)
// where the compositor and consumer support it, with a memfd/SHM fallback;
// the stream is BGRA and can carry cursor metadata (SPA_META_Cursor).
//
// The PipeWire producer mirrors KWin's screencast stream
// (src/plugins/screencast/screencaststream.cpp): PW_STREAM_FLAG_DRIVER |
// PW_STREAM_FLAG_ALLOC_BUFFERS, per-buffer allocation in add_buffer, and the
// same Meta set (Cursor, VideoDamage, Header).
//
// Protocol flow per session:
//   CreateSession -> SelectSources -> Start
// Start creates the PipeWire stream and the capture, then returns the stream node
// id in `streams`.
//
// This is the Serenity-style rewrite of tools/portal-screencast.c: it speaks
// D-Bus through Portal::/AK/LibCore (see dbus.h) instead of GDBus/GVariant, and
// there is no GLib/GIO in the plumbing. The PipeWire, Wayland, GBM and
// the capture code is unchanged.

#define _GNU_SOURCE

#include "screencast.h"
#include "introspection.h"

#include <AK/HashMap.h>
#include <AK/Random.h>
#include <AK/StringUtils.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gbm.h>
#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/param.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>

#define MAX_OUTPUTS 8
#define MAX_TOPLEVELS 64

// spa_pod_builder_add_object is a GCC statement-expression that yields a bare
// void*; give it a typed result for the `const struct spa_pod*` params array.
#define SDE_POD_ADD_OBJECT(...) static_cast<const struct spa_pod*>(spa_pod_builder_add_object(__VA_ARGS__))

namespace {

constexpr StringView screencast_interface = "org.freedesktop.impl.portal.ScreenCast"sv;

struct toplevel_info {
    struct ext_foreign_toplevel_handle_v1* handle;
    ByteString identifier;
    ByteString title;
};

// --- extension interface names (matched in the registry) --------------------

// --- state ------------------------------------------------------------------

struct screencast_state {
    HashMap<ByteString, struct session*> sessions; // session handle -> session

    // PipeWire
    struct pw_thread_loop* pw_loop;
    struct pw_context* pw_context;
    struct pw_core* pw_core;
    struct pw_registry* pw_registry;
    struct pw_stream* stream;
    int stream_width;
    int stream_height;
    int stream_node_id;
    u64 stream_serial;
    int core_sync_done;
    bool produced_first_buffer;
    // Negotiated stream format.
    bool negotiated;
    bool negotiated_dmabuf;
    uint32_t negotiated_spa_format;
    uint32_t negotiated_fourcc;
    uint64_t negotiated_modifier;
    uint64_t preferred_modifier;
    struct spa_hook stream_listener;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;

    // Wayland
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_shm* shm;
    struct zwp_linux_dmabuf_v1* dmabuf;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* capture_manager;
    struct ext_foreign_toplevel_list_v1* foreign_toplevel_list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1* foreign_source_manager;
    struct wl_output* outputs[MAX_OUTPUTS];
    size_t output_count;
    struct toplevel_info toplevels[MAX_TOPLEVELS];
    size_t toplevel_count;

    // GBM, for allocating DMA-BUF capture buffers.
    struct gbm_device* gbm;
    int gbm_fd;
    bool dmabuf_enabled;

    // Capture-session constraints (from ext-image-copy-capture): the DMA-BUF
    // format + modifiers the compositor can render into.
    bool capture_has_dmabuf;
    uint32_t capture_dmabuf_fourcc;
    uint64_t capture_dmabuf_modifiers[64];
    size_t capture_dmabuf_modifier_count;

    // Capture thread handshake: the PipeWire `process` callback queues a buffer
    // and waits while the Wayland thread renders one frame into it.
    pthread_mutex_t capture_mutex;
    pthread_cond_t capture_cond;
    int wake_pipe[2];
    bool wayland_running;
    struct capture* active_capture;
    struct portal_buffer* pending_buffer;
    int pending_status; // 0 = in flight, 1 = ready, -1 = failed
    bool capture_in_flight;
    uint64_t frame_sequence;

    // Cursor metadata (metadata cursor mode).
    bool cursor_metadata;
    struct ext_image_copy_capture_cursor_session_v1* cursor_session;
    struct ext_image_copy_capture_session_v1* cursor_capture;
    struct ext_image_copy_capture_frame_v1* cursor_frame;
    struct wl_buffer* cursor_buffer;
    uint8_t* cursor_map;
    size_t cursor_map_size;
    int cursor_width;
    int cursor_height;
    int cursor_stride;
    uint32_t cursor_shm_format;
    uint8_t* cursor_bitmap;
    bool cursor_bitmap_valid;
    bool cursor_bitmap_dirty;
    bool cursor_meta_logged;
    bool cursor_visible;
    int cursor_x;
    int cursor_y;
    int cursor_hotspot_x;
    int cursor_hotspot_y;
    int cursor_frames_since_refresh;

    pthread_t capture_thread;
    bool capture_thread_started;
};

// A PipeWire buffer, plus the backing memory and the wl_buffer the compositor
// renders into. Allocated in the stream add_buffer callback.
enum portal_buffer_kind {
    PORTAL_BUFFER_NONE,
    PORTAL_BUFFER_DMABUF,
    PORTAL_BUFFER_MEMFD,
};

struct portal_buffer {
    struct pw_buffer* pw_buffer;
    struct wl_buffer* wl_buffer;
    enum portal_buffer_kind kind;
    struct gbm_bo* bo;
    int fd;
    size_t map_size;
    int stride;
    uint32_t fourcc;
    uint64_t modifier;
};

struct session {
    ByteString handle;
    uint32_t types;
    uint32_t cursor_mode;
    bool started;
    int chosen_toplevel_index; // -1 = entire screen
    struct capture* capture;
    uint32_t persist_mode;
    ByteString restore_token;
};

struct capture {
    struct screencast_state* state;
    struct ext_image_copy_capture_session_v1* session;
    struct ext_image_copy_capture_frame_v1* frame;
    int width;
    int height;
    uint32_t shm_format;
    uint32_t source_type;
    ByteString source_id;
    uint32_t cursor_mode;
    bool first_frame_logged;
};

// `new capture {}` cannot be written where a local variable named `capture`
// shadows the type (the declarator is in scope in its initializer), so allocate
// through these small factories.
static struct capture* allocate_capture()
{
    return new capture {};
}

static struct session* allocate_session()
{
    return new session {};
}

static struct screencast_state g_state {};

// --- PipeWire ---------------------------------------------------------------

// DRM fourcc <-> PipeWire/SPA video format, mirroring KWin's table.
static uint32_t spa_video_format_to_fourcc(uint32_t spa_format)
{
    switch (spa_format) {
    case SPA_VIDEO_FORMAT_BGRA:
        return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx:
        return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBA:
        return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx:
        return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_ARGB:
        return DRM_FORMAT_BGRA8888;
    case SPA_VIDEO_FORMAT_xRGB:
        return DRM_FORMAT_BGRX8888;
    default:
        return DRM_FORMAT_INVALID;
    }
}

static uint32_t fourcc_to_spa_video_format(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
        return SPA_VIDEO_FORMAT_BGRA;
    case DRM_FORMAT_XRGB8888:
        return SPA_VIDEO_FORMAT_BGRx;
    case DRM_FORMAT_ABGR8888:
        return SPA_VIDEO_FORMAT_RGBA;
    case DRM_FORMAT_XBGR8888:
        return SPA_VIDEO_FORMAT_RGBx;
    case DRM_FORMAT_BGRA8888:
        return SPA_VIDEO_FORMAT_ARGB;
    case DRM_FORMAT_BGRX8888:
        return SPA_VIDEO_FORMAT_xRGB;
    default:
        return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

#define CURSOR_BPP 4
#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (size_t)(w) * (h) * CURSOR_BPP)
#define MAX_CURSOR_SIZE 256

static size_t cursor_meta_size(void)
{
    int w = g_state.cursor_width > 0 ? g_state.cursor_width : 64;
    int h = g_state.cursor_height > 0 ? g_state.cursor_height : 64;
    if (w > MAX_CURSOR_SIZE)
        w = MAX_CURSOR_SIZE;
    if (h > MAX_CURSOR_SIZE)
        h = MAX_CURSOR_SIZE;
    return CURSOR_META_SIZE(w, h);
}

// A wl_buffer around a DMA-BUF plane, via zwp_linux_dmabuf_v1.
static struct wl_buffer* create_dmabuf_wl_buffer(uint32_t fourcc, int width, int height,
    uint32_t offset, uint32_t stride, uint64_t modifier, int fd)
{
    if (!g_state.dmabuf)
        return nullptr;
    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g_state.dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
        (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffffu));
    struct wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return buffer;
}

// A wl_buffer around a memfd, via wl_shm.
static struct wl_buffer* create_shm_wl_buffer(int fd, int width, int height, int stride, uint32_t shm_format)
{
    if (!g_state.shm)
        return nullptr;
    struct wl_shm_pool* pool = wl_shm_create_pool(g_state.shm, fd, stride * height);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, shm_format);
    wl_shm_pool_destroy(pool);
    return buffer;
}

static void portal_buffer_destroy(struct portal_buffer* pcb)
{
    if (!pcb)
        return;
    if (pcb->wl_buffer)
        wl_buffer_destroy(pcb->wl_buffer);
    if (pcb->bo)
        gbm_bo_destroy(pcb->bo);
    if (pcb->fd >= 0)
        close(pcb->fd);
    free(pcb);
}

static void announce_stream_params(struct screencast_state* state);
static void write_cursor_meta(struct screencast_state* state, struct spa_buffer* spa_buffer);
static int build_enum_formats(struct screencast_state* state, struct spa_pod_builder* b,
    const struct spa_pod** params, int width, int height);

// Allocate the backing memory for a PipeWire buffer: a DMA-BUF (zero-copy) when
// the negotiated format carries a modifier, else a memfd. Mirrors KWin's
// add_buffer.
static void on_stream_add_buffer(void* data, struct pw_buffer* pw_buffer)
{
    struct screencast_state* state = static_cast<struct screencast_state*>(data);
    struct spa_buffer* spa_buffer = pw_buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas < 1)
        return;
    struct spa_data* spa_data = &spa_buffer->datas[0];

    struct portal_buffer* pcb = static_cast<struct portal_buffer*>(calloc(1, sizeof(struct portal_buffer)));
    pcb->pw_buffer = pw_buffer;
    pcb->fd = -1;

    if (state->negotiated_dmabuf && state->gbm && state->capture_has_dmabuf) {
        uint64_t modifiers[1] = { state->negotiated_modifier };
        pcb->bo = gbm_bo_create_with_modifiers(state->gbm, (uint32_t)state->stream_width,
            (uint32_t)state->stream_height, state->negotiated_fourcc, modifiers, 1);
        if (pcb->bo) {
            pcb->fd = gbm_bo_get_fd(pcb->bo);
            pcb->stride = (int)gbm_bo_get_stride(pcb->bo);
            pcb->fourcc = gbm_bo_get_format(pcb->bo);
            pcb->modifier = gbm_bo_get_modifier(pcb->bo);
            pcb->wl_buffer = create_dmabuf_wl_buffer(pcb->fourcc, state->stream_width, state->stream_height,
                gbm_bo_get_offset(pcb->bo, 0), (uint32_t)pcb->stride, pcb->modifier, pcb->fd);
            if (pcb->wl_buffer && pcb->fd >= 0 && pcb->stride > 0) {
                spa_data->type = SPA_DATA_DmaBuf;
                spa_data->flags = SPA_DATA_FLAG_READWRITE;
                spa_data->mapoffset = 0;
                spa_data->maxsize = pcb->stride * state->stream_height;
                spa_data->fd = pcb->fd;
                spa_data->data = nullptr;
                spa_data->chunk->offset = 0;
                spa_data->chunk->size = spa_data->maxsize;
                spa_data->chunk->stride = pcb->stride;
                spa_data->chunk->flags = SPA_CHUNK_FLAG_NONE;
                pcb->kind = PORTAL_BUFFER_DMABUF;
                pw_buffer->user_data = pcb;
                fprintf(stderr, "xdg-desktop-portal-serenity: screencast add_buffer DMA-BUF %dx%d fourcc=0x%08x modifier=0x%llx\n",
                    state->stream_width, state->stream_height, pcb->fourcc, (unsigned long long)pcb->modifier);
                return;
            }
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast DMA-BUF add_buffer failed (bo=%p wl=%p fd=%d stride=%d), falling back to memfd\n",
                (void*)pcb->bo, (void*)pcb->wl_buffer, pcb->fd, pcb->stride);
            portal_buffer_destroy(pcb);
            pcb = static_cast<struct portal_buffer*>(calloc(1, sizeof(struct portal_buffer)));
            pcb->pw_buffer = pw_buffer;
            pcb->fd = -1;
        }
    }

    // memfd fallback.
    int stride = state->stream_width * 4;
    size_t size = (size_t)stride * state->stream_height;
    int fd = memfd_create("sde-screencast", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        if (fd >= 0)
            close(fd);
        free(pcb);
        return;
    }
    pcb->fd = fd;
    pcb->stride = stride;
    pcb->fourcc = DRM_FORMAT_ARGB8888;
    pcb->map_size = size;
    pcb->wl_buffer = create_shm_wl_buffer(fd, state->stream_width, state->stream_height, stride, WL_SHM_FORMAT_ARGB8888);
    if (!pcb->wl_buffer) {
        portal_buffer_destroy(pcb);
        return;
    }
    spa_data->type = SPA_DATA_MemFd;
    spa_data->flags = SPA_DATA_FLAG_READWRITE;
    spa_data->mapoffset = 0;
    spa_data->maxsize = size;
    spa_data->fd = fd;
    spa_data->data = nullptr;
    spa_data->chunk->offset = 0;
    spa_data->chunk->size = size;
    spa_data->chunk->stride = stride;
    spa_data->chunk->flags = SPA_CHUNK_FLAG_NONE;
    pcb->kind = PORTAL_BUFFER_MEMFD;
    pw_buffer->user_data = pcb;
    fprintf(stderr, "xdg-desktop-portal-serenity: screencast add_buffer memfd %dx%d\n",
        state->stream_width, state->stream_height);
}

static void on_stream_remove_buffer(void* data, struct pw_buffer* pw_buffer)
{
    (void)data;
    struct portal_buffer* pcb = static_cast<struct portal_buffer*>(pw_buffer->user_data);
    if (pcb) {
        pw_buffer->user_data = nullptr;
        portal_buffer_destroy(pcb);
    }
}

static void write_stream_meta(struct screencast_state* state, struct spa_buffer* spa_buffer)
{
    struct spa_meta_header* header = static_cast<struct spa_meta_header*>(spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof(struct spa_meta_header)));
    if (header) {
        header->flags = 0;
        header->dts_offset = 0;
        header->seq = state->frame_sequence++;
        header->pts = (int64_t)pw_stream_get_nsec(state->stream);
    }

    // Damage: the whole frame each time (the compositor's damage is not yet
    // forwarded).
    struct spa_meta* damage = spa_buffer_find_meta(spa_buffer, SPA_META_VideoDamage);
    if (damage) {
        struct spa_meta_region* region = static_cast<struct spa_meta_region*>(spa_meta_first(damage));
        if (spa_meta_check(region, damage)) {
            region->region.position = SPA_POINT(0, 0);
            region->region.size = SPA_RECTANGLE((uint32_t)state->stream_width, (uint32_t)state->stream_height);
            region++;
        }
        if (spa_meta_check(region, damage)) {
            region->region.position = SPA_POINT(0, 0);
            region->region.size = SPA_RECTANGLE(0, 0);
        }
    }

    write_cursor_meta(state, spa_buffer);
}

static void on_stream_process(void* data)
{
    struct screencast_state* state = static_cast<struct screencast_state*>(data);
    struct pw_buffer* pw_buffer = pw_stream_dequeue_buffer(state->stream);
    if (!pw_buffer)
        return;
    struct spa_buffer* spa_buffer = pw_buffer->buffer;
    struct portal_buffer* pcb = static_cast<struct portal_buffer*>(pw_buffer->user_data);

    if (!pcb || !pcb->wl_buffer || !state->wayland_running) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast process without buffer (pcb=%p wl=%p running=%d)\n",
            (void*)pcb, pcb ? (void*)pcb->wl_buffer : nullptr, state->wayland_running);
        spa_buffer->datas[0].chunk->size = 0;
        pw_stream_queue_buffer(state->stream, pw_buffer);
        return;
    }

    // Ask the Wayland thread to render one frame into this buffer, and wait.
    pthread_mutex_lock(&state->capture_mutex);
    state->pending_buffer = pcb;
    state->pending_status = 0;
    ssize_t written = write(state->wake_pipe[1], "w", 1);
    (void)written;
    while (state->pending_status == 0 && state->wayland_running)
        pthread_cond_wait(&state->capture_cond, &state->capture_mutex);
    int status = state->pending_status;
    state->pending_buffer = nullptr;
    pthread_mutex_unlock(&state->capture_mutex);

    struct spa_data* spa_data = &spa_buffer->datas[0];
    if (status == 1) {
        spa_data->chunk->offset = 0;
        spa_data->chunk->size = spa_data->maxsize;
        spa_data->chunk->stride = pcb->stride;
        spa_data->chunk->flags = SPA_CHUNK_FLAG_NONE;
        write_stream_meta(state, spa_buffer);
        if (!state->produced_first_buffer) {
            state->produced_first_buffer = true;
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast first pipewire buffer (%dx%d, %s)\n",
                state->stream_width, state->stream_height,
                pcb->kind == PORTAL_BUFFER_DMABUF ? "DMA-BUF" : "memfd");
        }
    } else {
        spa_data->chunk->size = 0;
        spa_data->chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
    }

    pw_stream_queue_buffer(state->stream, pw_buffer);
}

static void on_stream_state_changed(void* data, enum pw_stream_state old, enum pw_stream_state state, char const* error)
{
    (void)old;
    struct screencast_state* s = static_cast<struct screencast_state*>(data);
    if (error)
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream error: %s\n", error);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        int node = pw_stream_get_node_id(s->stream);
        if (node > 0)
            s->stream_node_id = node;
    }
}

static bool test_dmabuf_modifier(struct screencast_state* state, uint64_t modifier)
{
    if (!state->gbm || state->stream_width <= 0 || state->stream_height <= 0)
        return false;
    uint32_t fourcc = state->capture_dmabuf_fourcc ? state->capture_dmabuf_fourcc : DRM_FORMAT_ARGB8888;
    uint64_t modifiers[1] = { modifier };
    struct gbm_bo* bo = gbm_bo_create_with_modifiers(state->gbm, (uint32_t)state->stream_width,
        (uint32_t)state->stream_height, fourcc, modifiers, 1);
    if (!bo)
        return false;
    gbm_bo_destroy(bo);
    return true;
}

static void on_stream_param_changed(void* data, uint32_t id, const struct spa_pod* param)
{
    struct screencast_state* state = static_cast<struct screencast_state*>(data);
    if (id != SPA_PARAM_Format || !param)
        return;
    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;

    state->stream_width = (int)info.size.width;
    state->stream_height = (int)info.size.height;
    state->negotiated_spa_format = info.format;
    state->negotiated_fourcc = spa_video_format_to_fourcc(info.format);
    if (state->negotiated_fourcc == DRM_FORMAT_INVALID)
        state->negotiated_fourcc = DRM_FORMAT_ARGB8888;

    if (info.flags & SPA_VIDEO_FLAG_MODIFIER) {
        // The stream offers a pre-fixated modifier; if the daemon has not
        // fixated it yet, adopt our preferred one.
        state->negotiated_dmabuf = state->gbm && state->capture_has_dmabuf;
        state->negotiated_modifier = (info.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED)
            ? state->preferred_modifier
            : info.modifier;
    } else {
        state->negotiated_dmabuf = false;
        state->negotiated_modifier = 0;
    }
    state->negotiated = true;

    fprintf(stderr, "xdg-desktop-portal-serenity: screencast negotiated %dx%d spa-format %d fourcc 0x%08x %s modifier=0x%llx\n",
        state->stream_width, state->stream_height, info.format, state->negotiated_fourcc,
        state->negotiated_dmabuf ? "DMA-BUF" : "memfd", (unsigned long long)state->negotiated_modifier);

    announce_stream_params(state);
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .add_buffer = on_stream_add_buffer,
    .remove_buffer = on_stream_remove_buffer,
    .process = on_stream_process,
};

// The core registry lets us read our node's object.serial (the v6 stream
// property clients should use for targeting).
static void on_registry_global(void* data, uint32_t id, uint32_t permissions, char const* type, uint32_t version, struct spa_dict const* props)
{
    (void)data;
    (void)id;
    (void)permissions;
    (void)type;
    (void)version;
    if (!props)
        return;
    char const* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    char const* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (serial && name && strcmp(name, "sde-screencast") == 0) {
        u64 value = AK::StringUtils::convert_to_uint<u64>(StringView { serial, strlen(serial) }).value_or(0);
        if (value)
            g_state.stream_serial = value;
    }
}
static void on_registry_global_remove(void* data, uint32_t id)
{
    (void)data;
    (void)id;
}
static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void on_core_done(void* data, uint32_t id, int seq)
{
    (void)id;
    (void)seq;
    static_cast<struct screencast_state*>(data)->core_sync_done = 1;
}
static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
};

// Choose a DMA-BUF modifier the compositor offers and GBM can allocate.
static uint64_t choose_dmabuf_modifier(struct screencast_state* state)
{
    for (size_t i = 0; i < state->capture_dmabuf_modifier_count; ++i) {
        if (state->capture_dmabuf_modifiers[i] == 0)
            return 0; // linear is the most portable
    }
    for (size_t i = 0; i < state->capture_dmabuf_modifier_count; ++i) {
        if (test_dmabuf_modifier(state, state->capture_dmabuf_modifiers[i]))
            return state->capture_dmabuf_modifiers[i];
    }
    return DRM_FORMAT_MOD_INVALID;
}

// Build the EnumFormat list: a pre-fixated DMA-BUF format (single modifier) plus
// a memfd fallback, offered up front so the link does not have to be
// renegotiated to fixate a modifier.
static int build_enum_formats(struct screencast_state* state, struct spa_pod_builder* b,
    const struct spa_pod** params, int width, int height)
{
    int n = 0;
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)width, (uint32_t)height);
    struct spa_rectangle size_min = SPA_RECTANGLE(1, 1);
    struct spa_rectangle size_max = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction rate = SPA_FRACTION(30, 1);

    if (state->capture_has_dmabuf && state->dmabuf && state->gbm && state->dmabuf_enabled && state->capture_dmabuf_modifier_count > 0) {
        uint64_t modifier = choose_dmabuf_modifier(state);
        uint32_t spa_format = fourcc_to_spa_video_format(state->capture_dmabuf_fourcc);
        if (spa_format == SPA_VIDEO_FORMAT_UNKNOWN)
            spa_format = SPA_VIDEO_FORMAT_BGRA;
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            state->preferred_modifier = modifier;
            struct spa_pod_frame frame;
            spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
            spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&size), SPA_POD_Rectangle(&size_min), SPA_POD_Rectangle(&size_max)), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate), 0);
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(b, modifier);
            params[n++] = static_cast<const struct spa_pod*>(spa_pod_builder_pop(b, &frame));
        }
    }

    // memfd fallback format, always offered.
    struct spa_pod_frame frame;
    spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&size), SPA_POD_Rectangle(&size_min), SPA_POD_Rectangle(&size_max)), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(SPA_POD_Fraction(&rate), SPA_POD_Fraction(&rate), SPA_POD_Fraction(&rate)), 0);
    params[n++] = static_cast<const struct spa_pod*>(spa_pod_builder_pop(b, &frame));
    return n;
}

static int start_pipewire_stream(int width, int height)
{
    g_state.stream_width = width;
    g_state.stream_height = height;
    if (!g_state.pw_loop) {
        pw_init(nullptr, nullptr);
        g_state.pw_loop = pw_thread_loop_new("sde-screencast", nullptr);
        if (!g_state.pw_loop) {
            fprintf(stderr, "xdg-desktop-portal-serenity: pw_thread_loop_new failed\n");
            return -1;
        }
        g_state.pw_context = pw_context_new(pw_thread_loop_get_loop(g_state.pw_loop), nullptr, 0);
        // Start the loop before connecting so the server can assign the node id.
        pw_thread_loop_start(g_state.pw_loop);
    }

    pw_thread_loop_lock(g_state.pw_loop);

    if (!g_state.pw_core && g_state.pw_context) {
        g_state.pw_core = pw_context_connect(g_state.pw_context, nullptr, 0);
        if (g_state.pw_core) {
            pw_core_add_listener(g_state.pw_core, &g_state.core_listener, &core_events, &g_state);
            g_state.pw_registry = pw_core_get_registry(g_state.pw_core, PW_VERSION_REGISTRY, 0);
            if (g_state.pw_registry)
                pw_registry_add_listener(g_state.pw_registry, &g_state.registry_listener, &registry_events, &g_state);
        }
    }

    // No PipeWire core means no PipeWire daemon: pw_stream_new() would dereference
    // the null core and take the whole portal down. Report it instead -- on a
    // machine without PipeWire, Start is expected to fail, not to crash.
    if (!g_state.pw_core) {
        fprintf(stderr, "xdg-desktop-portal-serenity: no PipeWire connection (is pipewire running?)\n");
        pw_thread_loop_unlock(g_state.pw_loop);
        return -1;
    }

    if (g_state.stream) {
        pw_stream_destroy(g_state.stream);
        g_state.stream = nullptr;
    }

    g_state.stream = pw_stream_new(
        g_state.pw_core,
        "SDE ScreenCast",
        pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_NODE_NAME, "sde-screencast",
            PW_KEY_NODE_DESCRIPTION, "SDE ScreenCast",
            PW_KEY_NODE_VIRTUAL, "false",
            nullptr));
    if (!g_state.stream) {
        fprintf(stderr, "xdg-desktop-portal-serenity: pw_stream_new failed\n");
        pw_thread_loop_unlock(g_state.pw_loop);
        return -1;
    }
    pw_stream_add_listener(g_state.stream, &g_state.stream_listener, &stream_events, &g_state);
    g_state.negotiated = false;

    uint8_t buffer[4096];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[8];
    int param_count = build_enum_formats(&g_state, &builder, params, width, height);
    // Also offer the buffer layout and metadata up front, so the server can
    // negotiate them together with the format.
    {
        int stride = width * 4;
        int buffertypes = (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd);
        params[param_count++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes));
        params[param_count++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
        params[param_count++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)sizeof(struct spa_meta_region) * 16,
                (int)sizeof(struct spa_meta_region), (int)sizeof(struct spa_meta_region) * 16));
        params[param_count++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_Int((int)cursor_meta_size()));
    }

    int result = pw_stream_connect(g_state.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_ALLOC_BUFFERS, (const struct spa_pod**)params, param_count);
    if (result < 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: pw_stream_connect failed: %d\n", result);
        pw_stream_destroy(g_state.stream);
        g_state.stream = nullptr;
        pw_thread_loop_unlock(g_state.pw_loop);
        return -1;
    }

    g_state.stream_width = width;
    g_state.stream_height = height;
    pw_stream_set_active(g_state.stream, true);
    // Force a roundtrip so the registry global (with object.serial) arrives.
    g_state.core_sync_done = 0;
    if (g_state.pw_core)
        pw_core_sync(g_state.pw_core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(g_state.pw_loop);

    // The node id and serial are assigned asynchronously; wait for them.
    for (int i = 0; i < 300 && (g_state.stream_node_id <= 0 || g_state.stream_serial == 0); ++i)
        usleep(5000);
    if (g_state.stream_node_id <= 0) {
        pw_thread_loop_lock(g_state.pw_loop);
        g_state.stream_node_id = pw_stream_get_node_id(g_state.stream);
        pw_thread_loop_unlock(g_state.pw_loop);
    }

    fprintf(stderr, "xdg-desktop-portal-serenity: screencast pipewire node id %d (serial %llu, %dx%d)\n",
        g_state.stream_node_id, (unsigned long long)g_state.stream_serial, width, height);
    return g_state.stream_node_id;
}

// Buffer requirements + metadata for the negotiated format. Called from
// param_changed (mirrors KWin's newStreamParams).
static void announce_stream_params(struct screencast_state* state)
{
    uint8_t buffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[4];
    int n = 0;

    int buffertypes = state->negotiated_dmabuf ? (1 << SPA_DATA_DmaBuf) : (1 << SPA_DATA_MemFd);
    int stride = state->stream_width * 4;

    struct spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8), 0);
    spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes), 0);
    if (state->negotiated_dmabuf) {
        spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * state->stream_height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), 0);
    } else {
        spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * state->stream_height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), 0);
    }
    params[n++] = static_cast<const struct spa_pod*>(spa_pod_builder_pop(&builder, &frame));

    params[n++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
    params[n++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)sizeof(struct spa_meta_region) * 16,
            (int)sizeof(struct spa_meta_region), (int)sizeof(struct spa_meta_region) * 16));
    if (state->cursor_metadata)
        params[n++] = SDE_POD_ADD_OBJECT(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_Int((int)cursor_meta_size()));

    pw_stream_update_params(state->stream, params, n);
}

// Fill the SPA_META_Cursor metadata for the metadata cursor mode: position and
// hotspot each frame, and the bitmap whenever it changed (mirrors KWin's
// addCursorMetadata).
static void write_cursor_meta(struct screencast_state* state, struct spa_buffer* spa_buffer)
{
    if (!state->cursor_metadata)
        return;
    struct spa_meta_cursor* cursor = static_cast<struct spa_meta_cursor*>(spa_buffer_find_meta_data(spa_buffer, SPA_META_Cursor, sizeof(struct spa_meta_cursor)));
    if (!cursor) {
        if (!state->cursor_meta_logged) {
            state->cursor_meta_logged = true;
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast no SPA_META_Cursor region (metadata=%d)\n", state->cursor_metadata);
        }
        return;
    }

    if (!state->cursor_visible) {
        cursor->id = 0;
        return;
    }

    cursor->id = 1;
    cursor->position.x = state->cursor_x;
    cursor->position.y = state->cursor_y;
    cursor->hotspot.x = state->cursor_hotspot_x;
    cursor->hotspot.y = state->cursor_hotspot_y;
    cursor->bitmap_offset = 0;

    if (!state->cursor_bitmap_dirty || !state->cursor_bitmap_valid)
        return;
    state->cursor_bitmap_dirty = false;
    cursor->bitmap_offset = (uint32_t)sizeof(struct spa_meta_cursor);

    struct spa_meta_bitmap* bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);
    bitmap->format = SPA_VIDEO_FORMAT_RGBA;
    bitmap->offset = (uint32_t)sizeof(struct spa_meta_bitmap);
    bitmap->size.width = (uint32_t)state->cursor_width;
    bitmap->size.height = (uint32_t)state->cursor_height;
    bitmap->stride = state->cursor_stride;

    uint8_t* data = SPA_MEMBER(bitmap, bitmap->offset, uint8_t);
    size_t row = (size_t)state->cursor_width * 4;
    size_t rows = (size_t)state->cursor_height;
    if ((uint8_t*)data + row * rows <= (uint8_t*)spa_meta_end(spa_buffer_find_meta(spa_buffer, SPA_META_Cursor))) {
        for (size_t y = 0; y < rows; ++y)
            memcpy(data + y * row, state->cursor_bitmap + y * (size_t)state->cursor_stride, row);
    }
}

// --- Wayland capture --------------------------------------------------------

// Complete the in-flight capture request (called on the Wayland thread).
static void complete_pending_capture(struct screencast_state* state, int status)
{
    pthread_mutex_lock(&state->capture_mutex);
    if (state->capture_in_flight) {
        state->capture_in_flight = false;
        state->pending_status = status;
        pthread_cond_broadcast(&state->capture_cond);
    }
    pthread_mutex_unlock(&state->capture_mutex);
}

static void frame_transform(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t transform)
{
    (void)data;
    (void)frame;
    (void)transform;
}
static void frame_damage(void* data, struct ext_image_copy_capture_frame_v1* frame, int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
static void frame_presentation_time(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
    (void)data;
    (void)frame;
    (void)sec_hi;
    (void)sec_lo;
    (void)nsec;
}

static void frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    struct capture* capture = static_cast<struct capture*>(data);
    if (!capture->first_frame_logged) {
        capture->first_frame_logged = true;
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast first frame %dx%d\n", capture->width, capture->height);
    }
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = nullptr;
    complete_pending_capture(capture->state, 1);
}

static void frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    struct capture* capture = static_cast<struct capture*>(data);
    (void)reason;
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = nullptr;
    complete_pending_capture(capture->state, -1);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void capture_session_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)session;
    struct capture* capture = static_cast<struct capture*>(data);
    capture->width = (int)width;
    capture->height = (int)height;
}
static void capture_session_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)session;
    struct capture* capture = static_cast<struct capture*>(data);
    if (capture->shm_format == 0)
        capture->shm_format = format;
}
static void capture_session_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}
static void capture_session_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)session;
    (void)data;
    // Remember the first DMA-BUF format the compositor can render into.
    if (g_state.capture_has_dmabuf)
        return;
    g_state.capture_dmabuf_fourcc = format;
    g_state.capture_dmabuf_modifier_count = 0;
    auto* entries = static_cast<uint64_t*>(modifiers->data);
    size_t entry_count = modifiers->size / sizeof(uint64_t);
    for (size_t i = 0; i < entry_count; ++i) {
        if (g_state.capture_dmabuf_modifier_count < 64)
            g_state.capture_dmabuf_modifiers[g_state.capture_dmabuf_modifier_count++] = entries[i];
    }
    g_state.capture_has_dmabuf = g_state.capture_dmabuf_modifier_count > 0;
}

static void setup_cursor_session(struct capture* capture);
static void cursor_capture_next(void);

static void capture_session_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    struct capture* capture = static_cast<struct capture*>(data);
    if (capture->width <= 0 || capture->height <= 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast session has no size\n");
        return;
    }
    // Set up the cursor session first, so the Cursor metadata is advertised
    // during stream negotiation. Then publish the PipeWire stream; frames are
    // produced on demand (one per PipeWire process()).
    if (capture->cursor_mode == 4)
        setup_cursor_session(capture);
    if (g_state.stream_node_id <= 0)
        start_pipewire_stream(capture->width, capture->height);
}
static void capture_session_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
}

static const struct ext_image_copy_capture_session_v1_listener capture_session_listener = {
    .buffer_size = capture_session_buffer_size,
    .shm_format = capture_session_shm_format,
    .dmabuf_device = capture_session_dmabuf_device,
    .dmabuf_format = capture_session_dmabuf_format,
    .done = capture_session_done,
    .stopped = capture_session_stopped,
};

// --- cursor metadata --------------------------------------------------------

static void cursor_session_enter(void* data, struct ext_image_copy_capture_cursor_session_v1* session)
{
    (void)data;
    (void)session;
    g_state.cursor_visible = true;
    cursor_capture_next();
}
static void cursor_session_leave(void* data, struct ext_image_copy_capture_cursor_session_v1* session)
{
    (void)data;
    (void)session;
    g_state.cursor_visible = false;
}
static void cursor_session_position(void* data, struct ext_image_copy_capture_cursor_session_v1* session, int32_t x, int32_t y)
{
    (void)data;
    (void)session;
    g_state.cursor_x = x;
    g_state.cursor_y = y;
}
static void cursor_session_hotspot(void* data, struct ext_image_copy_capture_cursor_session_v1* session, int32_t x, int32_t y)
{
    (void)data;
    (void)session;
    g_state.cursor_hotspot_x = x;
    g_state.cursor_hotspot_y = y;
}
static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_session_listener = {
    .enter = cursor_session_enter,
    .leave = cursor_session_leave,
    .position = cursor_session_position,
    .hotspot = cursor_session_hotspot,
};

static void cursor_capture_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)session;
    (void)data;
    g_state.cursor_width = (int)width;
    g_state.cursor_height = (int)height;
}
static void cursor_capture_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)session;
    (void)data;
    if (g_state.cursor_shm_format == 0)
        g_state.cursor_shm_format = format;
}
static void cursor_capture_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}
static void cursor_capture_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}
static void cursor_capture_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    (void)data;
    cursor_capture_next();
}
static void cursor_capture_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
}
static const struct ext_image_copy_capture_session_v1_listener cursor_capture_listener = {
    .buffer_size = cursor_capture_buffer_size,
    .shm_format = cursor_capture_shm_format,
    .dmabuf_device = cursor_capture_dmabuf_device,
    .dmabuf_format = cursor_capture_dmabuf_format,
    .done = cursor_capture_done,
    .stopped = cursor_capture_stopped,
};

static void cursor_capture_frame(void);

static void cursor_frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    (void)data;
    ext_image_copy_capture_frame_v1_destroy(frame);
    g_state.cursor_frame = nullptr;

    if (g_state.cursor_map && g_state.cursor_width > 0 && g_state.cursor_height > 0) {
        uint32_t fourcc = g_state.cursor_shm_format ? (uint32_t)g_state.cursor_shm_format : DRM_FORMAT_ARGB8888;
        uint32_t spa_format = fourcc_to_spa_video_format(fourcc);
        int stride = g_state.cursor_stride;
        size_t row = (size_t)g_state.cursor_width * 4;
        size_t rows = (size_t)g_state.cursor_height;
        uint8_t* dst = static_cast<uint8_t*>(realloc(g_state.cursor_bitmap, row * rows));
        if (dst) {
            g_state.cursor_bitmap = dst;
            for (size_t y = 0; y < rows; ++y) {
                uint8_t const* src = g_state.cursor_map + y * (size_t)stride;
                uint8_t* out = g_state.cursor_bitmap + y * row;
                for (int x = 0; x < g_state.cursor_width; ++x) {
                    // wl_shm ARGB8888 is premultiplied BGRA in memory; the meta
                    // bitmap is RGBA (as KWin sends it).
                    out[x * 4 + 0] = src[x * 4 + 2];
                    out[x * 4 + 1] = src[x * 4 + 1];
                    out[x * 4 + 2] = src[x * 4 + 0];
                    out[x * 4 + 3] = src[x * 4 + 3];
                }
            }
            g_state.cursor_bitmap_valid = true;
            g_state.cursor_bitmap_dirty = true;
            (void)spa_format;
        }
    }

    if (g_state.cursor_map) {
        munmap(g_state.cursor_map, g_state.cursor_map_size);
        g_state.cursor_map = nullptr;
    }
    if (g_state.cursor_buffer) {
        wl_buffer_destroy(g_state.cursor_buffer);
        g_state.cursor_buffer = nullptr;
    }
}
static void cursor_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)data;
    (void)reason;
    ext_image_copy_capture_frame_v1_destroy(frame);
    g_state.cursor_frame = nullptr;
    if (g_state.cursor_map) {
        munmap(g_state.cursor_map, g_state.cursor_map_size);
        g_state.cursor_map = nullptr;
    }
    if (g_state.cursor_buffer) {
        wl_buffer_destroy(g_state.cursor_buffer);
        g_state.cursor_buffer = nullptr;
    }
}
static const struct ext_image_copy_capture_frame_v1_listener cursor_frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = cursor_frame_ready,
    .failed = cursor_frame_failed,
};

// Capture the cursor bitmap (best effort, one frame at a time).
static void cursor_capture_frame(void)
{
    if (!g_state.cursor_capture || g_state.cursor_frame)
        return;
    if (g_state.cursor_width <= 0 || g_state.cursor_height <= 0)
        return;
    int stride = g_state.cursor_width * 4;
    size_t size = (size_t)stride * g_state.cursor_height;
    int fd = memfd_create("sde-cursor", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    uint8_t* map = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    uint32_t shm_format = g_state.cursor_shm_format ? g_state.cursor_shm_format : WL_SHM_FORMAT_ARGB8888;
    struct wl_shm_pool* pool = wl_shm_create_pool(g_state.shm, fd, (int)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, g_state.cursor_width, g_state.cursor_height, stride, shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    g_state.cursor_map = map;
    g_state.cursor_map_size = size;
    g_state.cursor_stride = stride;
    g_state.cursor_buffer = buffer;
    g_state.cursor_frame = ext_image_copy_capture_session_v1_create_frame(g_state.cursor_capture);
    ext_image_copy_capture_frame_v1_add_listener(g_state.cursor_frame, &cursor_frame_listener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(g_state.cursor_frame, buffer);
    ext_image_copy_capture_frame_v1_capture(g_state.cursor_frame);
    wl_display_flush(g_state.display);
}

static void cursor_capture_next(void)
{
    if (g_state.cursor_metadata && g_state.cursor_visible && !g_state.cursor_frame)
        cursor_capture_frame();
}

static void setup_cursor_session(struct capture* capture)
{
    if (!g_state.pointer || !g_state.capture_manager) {
        fprintf(stderr, "xdg-desktop-portal-serenity: metadata cursor requested but no wl_pointer\n");
        return;
    }
    struct ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(g_state.source_manager, g_state.outputs[0]);
    g_state.cursor_metadata = true;
    g_state.cursor_session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
        g_state.capture_manager, source, g_state.pointer);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_cursor_session_v1_add_listener(g_state.cursor_session, &cursor_session_listener, nullptr);
    g_state.cursor_capture = ext_image_copy_capture_cursor_session_v1_get_capture_session(g_state.cursor_session);
    ext_image_copy_capture_session_v1_add_listener(g_state.cursor_capture, &cursor_capture_listener, nullptr);
    fprintf(stderr, "xdg-desktop-portal-serenity: screencast metadata cursor session created\n");
    (void)capture;
}

static void ftl_handle_closed(void* data, struct ext_foreign_toplevel_handle_v1* handle)
{
    (void)handle;
    struct toplevel_info* toplevel = static_cast<struct toplevel_info*>(data);
    toplevel->handle = nullptr;
}
static void ftl_handle_done(void* data, struct ext_foreign_toplevel_handle_v1* handle)
{
    (void)data;
    (void)handle;
}
static void ftl_handle_title(void* data, struct ext_foreign_toplevel_handle_v1* handle, char const* title)
{
    (void)handle;
    struct toplevel_info* toplevel = static_cast<struct toplevel_info*>(data);
    toplevel->title = ByteString { title };
}
static void ftl_handle_app_id(void* data, struct ext_foreign_toplevel_handle_v1* handle, char const* app_id)
{
    (void)data;
    (void)handle;
    (void)app_id;
}
static void ftl_handle_identifier(void* data, struct ext_foreign_toplevel_handle_v1* handle, char const* identifier)
{
    (void)handle;
    struct toplevel_info* toplevel = static_cast<struct toplevel_info*>(data);
    toplevel->identifier = ByteString { identifier };
}
static const struct ext_foreign_toplevel_handle_v1_listener ftl_handle_listener = {
    .closed = ftl_handle_closed,
    .done = ftl_handle_done,
    .title = ftl_handle_title,
    .app_id = ftl_handle_app_id,
    .identifier = ftl_handle_identifier,
};

static void ftl_toplevel(void* data, struct ext_foreign_toplevel_list_v1* list, struct ext_foreign_toplevel_handle_v1* handle)
{
    (void)list;
    struct screencast_state* state = static_cast<struct screencast_state*>(data);
    if (state->toplevel_count >= MAX_TOPLEVELS) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    struct toplevel_info* toplevel = &state->toplevels[state->toplevel_count++];
    toplevel->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &ftl_handle_listener, toplevel);
}
static void ftl_finished(void* data, struct ext_foreign_toplevel_list_v1* list)
{
    (void)data;
    (void)list;
}
static const struct ext_foreign_toplevel_list_v1_listener ftl_listener = {
    .toplevel = ftl_toplevel,
    .finished = ftl_finished,
};

static void seat_capabilities(void* data, struct wl_seat* seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_state.pointer)
        g_state.pointer = wl_seat_get_pointer(seat);
}
static void seat_name(void* data, struct wl_seat* seat, char const* name)
{
    (void)data;
    (void)seat;
    (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
        g_state.shm = static_cast<struct wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g_state.dmabuf = static_cast<struct zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4));
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !g_state.seat) {
        g_state.seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7));
        wl_seat_add_listener(g_state.seat, &seat_listener, nullptr);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g_state.source_manager = static_cast<struct ext_output_image_capture_source_manager_v1*>(wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g_state.capture_manager = static_cast<struct ext_image_copy_capture_manager_v1*>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0)
        g_state.foreign_source_manager = static_cast<struct ext_foreign_toplevel_image_capture_source_manager_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        g_state.foreign_toplevel_list = static_cast<struct ext_foreign_toplevel_list_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(g_state.foreign_toplevel_list, &ftl_listener, &g_state);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && g_state.output_count < MAX_OUTPUTS) {
        g_state.outputs[g_state.output_count++] = static_cast<struct wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, version));
    }
}
static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Start the pending capture request (a PipeWire process() waiting on a frame).
// Runs on the Wayland thread, which owns the capture session.
static void begin_pending_capture(struct screencast_state* state)
{
    pthread_mutex_lock(&state->capture_mutex);
    if (!state->pending_buffer || state->capture_in_flight || !state->active_capture
        || state->active_capture->frame || !state->active_capture->session) {
        pthread_mutex_unlock(&state->capture_mutex);
        return;
    }
    struct portal_buffer* pcb = state->pending_buffer;
    struct capture* capture = state->active_capture;
    state->capture_in_flight = true;
    pthread_mutex_unlock(&state->capture_mutex);

    capture->frame = ext_image_copy_capture_session_v1_create_frame(capture->session);
    ext_image_copy_capture_frame_v1_add_listener(capture->frame, &frame_listener, capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture->frame, pcb->wl_buffer);
    ext_image_copy_capture_frame_v1_capture(capture->frame);
}

static void* capture_thread(void* data)
{
    struct screencast_state* state = static_cast<struct screencast_state*>(data);
    while (state->wayland_running) {
        begin_pending_capture(state);
        wl_display_dispatch_pending(state->display);
        while (wl_display_prepare_read(state->display) != 0) {
            if (wl_display_dispatch_pending(state->display) < 0)
                return nullptr;
        }
        wl_display_flush(state->display);
        struct pollfd fds[2] = {
            { wl_display_get_fd(state->display), POLLIN, 0 },
            { state->wake_pipe[0], POLLIN, 0 },
        };
        int n = poll(fds, 2, 200);
        if (n < 0) {
            wl_display_cancel_read(state->display);
            if (errno == EINTR)
                continue;
            return nullptr;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (wl_display_read_events(state->display) < 0)
                return nullptr;
            wl_display_dispatch_pending(state->display);
        } else {
            wl_display_cancel_read(state->display);
        }
        if (fds[1].revents & POLLIN) {
            char drain[128];
            while (read(state->wake_pipe[0], drain, sizeof(drain)) > 0) {
            }
        }
    }
    return nullptr;
}

// Set up the Wayland connection (idempotent).
static bool ensure_wayland(struct screencast_state* state)
{
    if (state->display)
        return true;

    pthread_mutex_init(&state->capture_mutex, nullptr);
    pthread_cond_init(&state->capture_cond, nullptr);
    if (pipe(state->wake_pipe) != 0)
        return false;
    fcntl(state->wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(state->wake_pipe[1], F_SETFL, O_NONBLOCK);
    state->wayland_running = true;

    // GBM device for allocating DMA-BUF capture buffers (zero-copy).
    char const* node = getenv("SDE_SCREENCAST_RENDER_NODE");
    if (!node || !*node)
        node = getenv("WLR_RENDER_DRM_DEVICE");
    if (!node || !*node)
        node = "/dev/dri/renderD128";
    state->gbm_fd = open(node, O_RDWR | O_CLOEXEC);
    if (state->gbm_fd >= 0) {
        state->gbm = gbm_create_device(state->gbm_fd);
        if (!state->gbm) {
            close(state->gbm_fd);
            state->gbm_fd = -1;
        }
    }
    if (!state->gbm)
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast could not open GBM (%s); using memfd\n", node);
    // DMA-BUF capture is preferred where the compositor/PipeWire support it;
    // the stream always offers a memfd fallback, so it degrades safely. Set
    // SDE_SCREENCAST_NO_DMABUF to force memfd.
    state->dmabuf_enabled = getenv("SDE_SCREENCAST_NO_DMABUF") == nullptr;

    state->display = wl_display_connect(nullptr);
    if (!state->display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast cannot connect to Wayland\n");
        return false;
    }
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    wl_display_roundtrip(state->display); // globals
    wl_display_roundtrip(state->display); // seat capabilities
    wl_display_roundtrip(state->display); // output/toplevel info
    return true;
}

// Run the LibGUI source chooser (SDE_PORTAL_CHOOSER) with the candidate labels;
// returns the chosen index, or 0 when there is no chooser.
static int run_source_chooser(Vector<ByteString> const& labels)
{
    auto chooser = Core::Environment::get("SDE_PORTAL_CHOOSER"sv);
    if (!chooser.has_value() || chooser->is_empty() || labels.is_empty())
        return 0;

    Vector<ByteString> arguments;
    arguments.append(chooser->to_byte_string());
    arguments.append("-t"sv);
    arguments.append("Share your screen"sv);
    for (auto const& label : labels)
        arguments.append(label);

    Vector<char const*> argv;
    argv.ensure_capacity(arguments.size() + 1);
    for (auto const& argument : arguments)
        argv.append(argument.characters());
    argv.append(nullptr);

    int pipe_fds[2] { -1, -1 };
    if (pipe(pipe_fds) != 0)
        return 0;

    auto pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execvp(argv[0], const_cast<char**>(argv.data()));
        _exit(127);
    }
    close(pipe_fds[1]);

    ByteString output;
    if (auto file = Core::File::adopt_fd(pipe_fds[0], Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            auto bytes = contents.release_value();
            output = ByteString { StringView { reinterpret_cast<char const*>(bytes.data()), bytes.size() } };
        }
    }

    int wait_status = 0;
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR)
        ;

    int chosen = 0;
    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
        chosen = atoi(output.characters());
    if (chosen < 0 || chosen >= static_cast<int>(labels.size()))
        chosen = 0;
    return chosen;
}

// Set up the capture for the requested source (the chosen window, else the
// primary output); returns the capture, or nullptr.
static struct capture* begin_capture(struct screencast_state* state, struct session* session)
{
    if (!ensure_wayland(state))
        return nullptr;
    if (!state->shm || !state->source_manager || !state->capture_manager || state->output_count == 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast missing capture globals (shm=%p sm=%p cm=%p outputs=%zu)\n",
            (void*)state->shm, (void*)state->source_manager, (void*)state->capture_manager, state->output_count);
        return nullptr;
    }

    struct capture* capture = allocate_capture();
    capture->state = state;
    capture->shm_format = WL_SHM_FORMAT_XRGB8888;
    capture->cursor_mode = session ? session->cursor_mode : 1;

    // Reset the DMA-BUF constraint info for this session.
    state->capture_has_dmabuf = false;
    state->capture_dmabuf_modifier_count = 0;

    // A chosen window when the compositor offers it; otherwise the primary output.
    struct ext_image_capture_source_v1* source = nullptr;
    if (session && session->chosen_toplevel_index >= 0 && state->foreign_source_manager) {
        struct toplevel_info* toplevel = &state->toplevels[session->chosen_toplevel_index];
        if (toplevel->handle) {
            source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(state->foreign_source_manager, toplevel->handle);
            capture->source_type = 2;
            capture->source_id = toplevel->identifier;
        }
    }
    if (!source) {
        source = ext_output_image_capture_source_manager_v1_create_source(state->source_manager, state->outputs[0]);
        capture->source_type = 1;
    }

    // cursor_mode 2 = embedded: ask the compositor to paint the cursor into the
    // capture (niri's "embedded" mode). 4 = metadata: capture the cursor
    // separately and describe it with SPA_META_Cursor (setup once the size is
    // known, in capture_session_done).
    uint32_t options = (capture->cursor_mode == 2) ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    capture->session = ext_image_copy_capture_manager_v1_create_session(state->capture_manager, source, options);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(capture->session, &capture_session_listener, capture);

    state->active_capture = capture;
    if (!state->capture_thread_started) {
        state->capture_thread_started = true;
        pthread_create(&state->capture_thread, nullptr, capture_thread, state);
    }
    wl_display_flush(state->display);
    return capture;
}

// --- restore tokens ---------------------------------------------------------
//
// The XDG ScreenCast restore token lets an application remember a source so the
// user is not asked to pick again. Tokens are persisted as one pipe-separated
// record per line ("token|types|cursor_mode|source_id") in the SDE state
// directory.

ByteString screencast_state_path(StringView name)
{
    auto state = Core::Environment::get("SDE_STATE_DIR"sv);
    ByteString base;
    if (state.has_value() && !state->is_empty()) {
        base = state->to_byte_string();
    } else {
        auto config = Core::Environment::get("XDG_CONFIG_HOME"sv);
        if (config.has_value() && !config->is_empty()) {
            base = ByteString::formatted("{}/sde", config->to_byte_string());
        } else {
            auto home = Core::Environment::get("HOME"sv);
            ByteString home_path = home.has_value() ? home->to_byte_string() : ByteString { "/tmp" };
            base = ByteString::formatted("{}/.config/sde", home_path);
        }
    }
    // Create the state directory (and its parents) if needed.
    (void)Core::Directory::create(base, Core::Directory::CreateDirectories::Yes, 0700);
    return ByteString::formatted("{}/{}", base, name);
}

// --- D-Bus interface --------------------------------------------------------

static struct session* find_session(ByteString const& handle)
{
    if (auto found = g_state.sessions.get(handle); found.has_value())
        return found.value();
    return nullptr;
}

static void handle_screencast(Portal::MethodCall& call)
{
    if (call.member() == "CreateSession"sv) {
        auto session_handle = call.object_path_argument(1);
        auto* session = allocate_session();
        session->handle = session_handle;
        session->types = 1; // monitor
        session->cursor_mode = 1;
        session->chosen_toplevel_index = -1;
        g_state.sessions.set(session_handle, session);
        call.reply_response(0);
        return;
    }

    if (call.member() == "SelectSources"sv) {
        auto session_handle = call.object_path_argument(1);
        struct session* session = find_session(session_handle);
        auto const* options = call.argument(3);
        if (session && options && options->is(Portal::Value::Type::Dict)) {
            session->types = options->uint32_or("types"sv, session->types);
            session->cursor_mode = options->uint32_or("cursor_mode"sv, session->cursor_mode);
            session->persist_mode = options->uint32_or("persist_mode"sv, session->persist_mode);

            // Restore a previously persisted source if the app supplies a token.
            auto restore = options->string_or("restore_token"sv);
            if (!restore.is_empty()) {
                if (auto stored = Screencast::load_restore_token(restore); stored.has_value()) {
                    session->types = stored->types;
                    session->cursor_mode = stored->cursor_mode;
                    session->restore_token = restore;
                    if (!stored->source_id.is_empty()) {
                        ensure_wayland(&g_state);
                        for (size_t i = 0; i < g_state.toplevel_count; ++i) {
                            if (g_state.toplevels[i].identifier == stored->source_id) {
                                session->chosen_toplevel_index = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                }
                if (!session->restore_token.is_empty()) {
                    Portal::Dictionary results;
                    results.append(Portal::Value::Entry { "restore_token"sv, Portal::Value::make_variant(Portal::Value::make_string(session->restore_token)) });
                    call.reply_response(0, results);
                    return;
                }
            }
        }
        // When window sources are offered and a chooser is configured, let the
        // user pick via the LibGUI ScreenCastChooser (index 0 = entire screen).
        auto chooser = Core::Environment::get("SDE_PORTAL_CHOOSER"sv);
        if (session && (session->types & 2) && chooser.has_value() && !chooser->is_empty()) {
            ensure_wayland(&g_state);
            Vector<ByteString> labels;
            labels.append("Entire screen"sv);
            for (size_t i = 0; i < g_state.toplevel_count; ++i) {
                labels.append(ByteString::formatted("Window: {}", g_state.toplevels[i].title.is_empty()
                        ? (g_state.toplevels[i].identifier.is_empty() ? ByteString { "Window" } : g_state.toplevels[i].identifier)
                        : g_state.toplevels[i].title));
            }
            int chosen = run_source_chooser(labels);
            session->chosen_toplevel_index = chosen - 1; // -1 => entire screen
        }

        // Mint and persist a restore token when the app asked to remember the
        // choice (persist_mode 1 = permanent, 2 = until revoked).
        if (session && session->persist_mode != 0 && session->restore_token.is_empty()) {
            char token[33];
            for (int i = 0; i < 32; ++i)
                token[i] = "0123456789abcdef"[AK::get_random_uniform(16)];
            token[32] = '\0';
            ByteString source_id;
            if (session->chosen_toplevel_index >= 0 && (size_t)session->chosen_toplevel_index < g_state.toplevel_count)
                source_id = g_state.toplevels[session->chosen_toplevel_index].identifier;
            Screencast::store_restore_token(ByteString { token }, session->types, session->cursor_mode, source_id);
            session->restore_token = token;
        }
        Portal::Dictionary results;
        if (session && !session->restore_token.is_empty())
            results.append(Portal::Value::Entry { "restore_token"sv, Portal::Value::make_variant(Portal::Value::make_string(session->restore_token)) });
        call.reply_response(0, results);
        return;
    }

    if (call.member() == "Start"sv) {
        auto session_handle = call.object_path_argument(1);
        struct session* session = find_session(session_handle);
        if (!session) {
            call.reply_response(2);
            return;
        }
        session->capture = begin_capture(&g_state, session);
        if (!session->capture) {
            call.reply_response(2);
            return;
        }
        // Wait for the stream (created when the capture session reports its size).
        for (int i = 0; i < 100 && g_state.stream_node_id <= 0; ++i) {
            wl_display_flush(g_state.display);
            usleep(10000);
        }
        if (g_state.stream_node_id > 0) {
            Portal::Dictionary props;
            props.append(Portal::Value::Entry { "position"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(0), Portal::Value::make_int32(0) })) });
            props.append(Portal::Value::Entry { "size"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(g_state.stream_width), Portal::Value::make_int32(g_state.stream_height) })) });
            props.append(Portal::Value::Entry { "source_type"sv, Portal::Value::make_variant(Portal::Value::make_uint32(session->capture->source_type)) });
            if (!session->capture->source_id.is_empty())
                props.append(Portal::Value::Entry { "id"sv, Portal::Value::make_variant(Portal::Value::make_string(session->capture->source_id)) });
            if (g_state.stream_serial)
                props.append(Portal::Value::Entry { "pipewire-serial"sv, Portal::Value::make_variant(Portal::Value::make_uint64(g_state.stream_serial)) });

            Portal::Value::Container stream;
            stream.append(Portal::Value::make_uint32(static_cast<u32>(g_state.stream_node_id)));
            stream.append(Portal::Value::make_dict(move(props)));
            auto streams = Portal::Value::make_array({ Portal::Value::make_struct(move(stream)) }, "(ua{sv})"sv);

            Portal::Dictionary results;
            results.append(Portal::Value::Entry { "streams"sv, Portal::Value::make_variant(move(streams)) });
            if (!session->restore_token.is_empty())
                results.append(Portal::Value::Entry { "restore_token"sv, Portal::Value::make_variant(Portal::Value::make_string(session->restore_token)) });
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream node %d (%dx%d, source_type %u, cursor %u)\n",
                g_state.stream_node_id, g_state.stream_width, g_state.stream_height,
                session->capture->source_type, session->capture->cursor_mode);
            call.reply_response(0, results);
        } else {
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream not ready\n");
            call.reply_response(2);
        }
        return;
    }

    if (call.member() == "AvailableSourceTypes"sv) {
        Portal::Dictionary results;
        results.append(Portal::Value::Entry { "types"sv, Portal::Value::make_variant(Portal::Value::make_uint32(3)) }); // monitor | window
        call.reply_response(0, results);
        return;
    }

    if (call.member() == "AvailableCursorModes"sv) {
        Portal::Dictionary results;
        results.append(Portal::Value::Entry { "cursor_modes"sv, Portal::Value::make_variant(Portal::Value::make_uint32(7)) }); // hidden | embedded | metadata
        call.reply_response(0, results);
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

// --- one-shot screenshot (Screenshot portal) --------------------------------

struct screenshot_capture {
    int width;
    int height;
    uint32_t shm_format;
    uint8_t* pixels;
    size_t size;
    int stride;
    struct wl_shm* shm;
    struct wl_buffer* buffer;
    struct ext_image_copy_capture_frame_v1* frame;
    int status; // 0 pending, 1 ready, -1 failed
};

static void shot_session_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)session;
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    shot->width = (int)width;
    shot->height = (int)height;
}
static void shot_session_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)session;
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    if (shot->shm_format == 0)
        shot->shm_format = format;
}
static void shot_session_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}
static void shot_session_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}
static void shot_frame_transform(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t transform)
{
    (void)data;
    (void)frame;
    (void)transform;
}
static void shot_frame_damage(void* data, struct ext_image_copy_capture_frame_v1* frame, int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
static void shot_frame_presentation_time(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
    (void)data;
    (void)frame;
    (void)sec_hi;
    (void)sec_lo;
    (void)nsec;
}
static void shot_frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    shot->status = 1;
    ext_image_copy_capture_frame_v1_destroy(frame);
    shot->frame = nullptr;
}
static void shot_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)reason;
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    shot->status = -1;
    ext_image_copy_capture_frame_v1_destroy(frame);
    shot->frame = nullptr;
}
static const struct ext_image_copy_capture_frame_v1_listener shot_frame_listener = {
    .transform = shot_frame_transform,
    .damage = shot_frame_damage,
    .presentation_time = shot_frame_presentation_time,
    .ready = shot_frame_ready,
    .failed = shot_frame_failed,
};
static void shot_session_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    if (shot->width <= 0 || shot->height <= 0 || shot->shm_format == 0) {
        shot->status = -1;
        return;
    }
    shot->stride = shot->width * 4;
    shot->size = (size_t)shot->stride * shot->height;
    int fd = memfd_create("sde-screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)shot->size) != 0) {
        shot->status = -1;
        if (fd >= 0)
            close(fd);
        return;
    }
    shot->pixels = static_cast<uint8_t*>(mmap(nullptr, shot->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (shot->pixels == MAP_FAILED) {
        shot->pixels = nullptr;
        shot->status = -1;
        close(fd);
        return;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(shot->shm, fd, (int)shot->size);
    shot->buffer = wl_shm_pool_create_buffer(pool, 0, shot->width, shot->height, shot->stride, shot->shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    shot->frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(shot->frame, &shot_frame_listener, shot);
    ext_image_copy_capture_frame_v1_attach_buffer(shot->frame, shot->buffer);
    ext_image_copy_capture_frame_v1_capture(shot->frame);
}
static void shot_session_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    struct screenshot_capture* shot = static_cast<struct screenshot_capture*>(data);
    if (shot->status == 0)
        shot->status = -1;
}
static const struct ext_image_copy_capture_session_v1_listener shot_session_listener = {
    .buffer_size = shot_session_buffer_size,
    .shm_format = shot_session_shm_format,
    .dmabuf_device = shot_session_dmabuf_device,
    .dmabuf_format = shot_session_dmabuf_format,
    .done = shot_session_done,
    .stopped = shot_session_stopped,
};

// A dedicated registry for the one-shot screenshot connection.
struct shot_globals {
    struct wl_shm* shm;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* capture_manager;
    struct wl_output* output;
};

static void shot_registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    struct shot_globals* globals = static_cast<struct shot_globals*>(data);
    if (strcmp(interface, wl_shm_interface.name) == 0)
        globals->shm = static_cast<struct wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        globals->source_manager = static_cast<struct ext_output_image_capture_source_manager_v1*>(wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        globals->capture_manager = static_cast<struct ext_image_copy_capture_manager_v1*>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    else if (strcmp(interface, wl_output_interface.name) == 0 && !globals->output)
        globals->output = static_cast<struct wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, version));
}
static void shot_registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener shot_registry_listener = {
    .global = shot_registry_global,
    .global_remove = shot_registry_global_remove,
};

}

namespace Screencast {

void register_interface(Portal::Service& service)
{
    service.register_interface(
        screencast_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_screencast(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(6);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(screencast_interface).value_or(StringView {}));
}

void store_restore_token(ByteString const& token, u32 types, u32 cursor_mode, ByteString const& source_id)
{
    auto path = screencast_state_path("screencast-tokens"sv);
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Append);
    if (file.is_error())
        return;
    (void)file.value()->write_formatted("{}|{}|{}|{}\n", token, types, cursor_mode, source_id);
}

Optional<RestoreToken> load_restore_token(ByteString const& token)
{
    auto path = screencast_state_path("screencast-tokens"sv);
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto bytes = contents.release_value();
    StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };

    for (auto line : text.split_view('\n')) {
        if (line.is_empty())
            continue;
        auto first = line.find('|');
        if (!first.has_value())
            continue;
        if (line.substring_view(0, *first) != token)
            continue;
        auto rest = line.substring_view(*first + 1);
        auto second = rest.find('|');
        if (!second.has_value())
            continue;
        auto rest2 = rest.substring_view(*second + 1);
        auto third = rest2.find('|');
        if (!third.has_value())
            continue;

        RestoreToken stored;
        stored.types = AK::StringUtils::convert_to_uint<u32>(rest.substring_view(0, *second)).value_or(0);
        stored.cursor_mode = AK::StringUtils::convert_to_uint<u32>(rest2.substring_view(0, *third)).value_or(0);
        stored.source_id = ByteString { rest2.substring_view(*third + 1) };
        return stored;
    }
    return {};
}

int capture_png(ByteString const& path)
{
    // A dedicated Wayland connection, so a one-shot capture never races the
    // streaming capture thread on the shared display.
    struct wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screenshot cannot connect to Wayland\n");
        return -1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    struct shot_globals globals {};
    wl_registry_add_listener(registry, &shot_registry_listener, &globals);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!globals.shm || !globals.source_manager || !globals.capture_manager || !globals.output) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screenshot missing capture globals\n");
        wl_display_disconnect(display);
        return -1;
    }

    struct screenshot_capture shot {};
    shot.shm = globals.shm;
    struct ext_image_capture_source_v1* source = ext_output_image_capture_source_manager_v1_create_source(globals.source_manager, globals.output);
    struct ext_image_copy_capture_session_v1* session = ext_image_copy_capture_manager_v1_create_session(globals.capture_manager, source, 0);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(session, &shot_session_listener, &shot);

    for (int i = 0; i < 300 && shot.status == 0; ++i) {
        wl_display_flush(display);
        struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
        if (poll(&pfd, 1, 20) > 0)
            wl_display_dispatch(display);
        else
            wl_display_dispatch_pending(display);
    }

    int result = -1;
    if (shot.status == 1 && shot.pixels && shot.width > 0 && shot.height > 0) {
        // The SHM frame is BGRA8888 in memory; wrap it (no copy) and encode.
        auto bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, { shot.width, shot.height }, 1, shot.stride, shot.pixels);
        if (!bitmap.is_error()) {
            auto png = Gfx::PNGWriter::encode(*bitmap.value());
            if (!png.is_error()) {
                if (auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !file.is_error()) {
                    if (!file.value()->write_until_depleted(png.value()).is_error())
                        result = 0;
                }
            }
        }
    }

    if (shot.frame)
        ext_image_copy_capture_frame_v1_destroy(shot.frame);
    if (shot.buffer)
        wl_buffer_destroy(shot.buffer);
    if (shot.pixels)
        munmap(shot.pixels, shot.size);
    ext_image_copy_capture_session_v1_destroy(session);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return result;
}

int start_for_session(ByteString const& session_handle, Stream& out)
{
    struct session* session = find_session(session_handle);
    if (!session)
        return -1;
    session->capture = begin_capture(&g_state, session);
    if (!session->capture)
        return -1;
    for (int i = 0; i < 100 && g_state.stream_node_id <= 0; ++i) {
        wl_display_flush(g_state.display);
        usleep(10000);
    }
    if (g_state.stream_node_id <= 0)
        return -1;
    out.node_id = static_cast<u32>(g_state.stream_node_id);
    out.width = g_state.stream_width;
    out.height = g_state.stream_height;
    out.source_type = session->capture->source_type;
    return 0;
}

}
