/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 ScreenCast backend for xdg-desktop-portal-serenity.
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

#define _GNU_SOURCE

#include "portal-screencast.h"

#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define MAX_OUTPUTS 8
#define MAX_TOPLEVELS 64

struct toplevel_info {
    struct ext_foreign_toplevel_handle_v1* handle;
    char* identifier;
    char* title;
};

// --- extension interface names (matched in the registry) --------------------

// --- state ------------------------------------------------------------------

struct screencast_state {
    GHashTable* sessions; // session handle -> Session*

    // PipeWire
    struct pw_thread_loop* pw_loop;
    struct pw_context* pw_context;
    struct pw_core* pw_core;
    struct pw_registry* pw_registry;
    struct pw_stream* stream;
    int stream_width;
    int stream_height;
    int stream_node_id;
    guint64 stream_serial;
    int core_sync_done;
    gboolean produced_first_buffer;
    // Negotiated stream format.
    gboolean negotiated;
    gboolean negotiated_dmabuf;
    uint32_t negotiated_spa_format;
    uint32_t negotiated_fourcc;
    uint64_t negotiated_modifier;
    gboolean fixate_offered;
    uint64_t fixate_modifier;
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
    gboolean dmabuf_enabled;

    // Capture-session constraints (from ext-image-copy-capture): the DMA-BUF
    // format + modifiers the compositor can render into.
    gboolean capture_has_dmabuf;
    uint32_t capture_dmabuf_fourcc;
    uint64_t capture_dmabuf_modifiers[64];
    size_t capture_dmabuf_modifier_count;

    // Capture thread handshake: the PipeWire `process` callback queues a buffer
    // and waits while the Wayland thread renders one frame into it.
    pthread_mutex_t capture_mutex;
    pthread_cond_t capture_cond;
    int wake_pipe[2];
    gboolean wayland_running;
    struct capture* active_capture;
    struct portal_buffer* pending_buffer;
    int pending_status; // 0 = in flight, 1 = ready, -1 = failed
    gboolean capture_in_flight;
    uint64_t frame_sequence;

    // Cursor metadata (metadata cursor mode).
    gboolean cursor_metadata;
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
    gboolean cursor_bitmap_valid;
    gboolean cursor_bitmap_dirty;
    gboolean cursor_meta_logged;
    gboolean cursor_visible;
    int cursor_x;
    int cursor_y;
    int cursor_hotspot_x;
    int cursor_hotspot_y;
    int cursor_frames_since_refresh;

    pthread_t capture_thread;
    gboolean capture_thread_started;
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
    char* handle;
    uint32_t types;
    uint32_t cursor_mode;
    gboolean started;
    int chosen_toplevel_index; // -1 = entire screen
    struct capture* capture;
    uint32_t persist_mode;
    char* restore_token;
};

struct capture {
    struct screencast_state* state;
    struct ext_image_copy_capture_session_v1* session;
    struct ext_image_copy_capture_frame_v1* frame;
    int width;
    int height;
    uint32_t shm_format;
    uint32_t source_type;
    char* source_id;
    uint32_t cursor_mode;
    gboolean first_frame_logged;
};

static struct screencast_state g_state = { 0 };

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
        return NULL;
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
        return NULL;
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
    const struct spa_pod** params, gboolean fixate, uint64_t fixate_modifier, int width, int height);

// Allocate the backing memory for a PipeWire buffer: a DMA-BUF (zero-copy) when
// the negotiated format carries a modifier, else a memfd. Mirrors KWin's
// add_buffer.
static void on_stream_add_buffer(void* data, struct pw_buffer* pw_buffer)
{
    struct screencast_state* state = data;
    struct spa_buffer* spa_buffer = pw_buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas < 1)
        return;
    struct spa_data* spa_data = &spa_buffer->datas[0];

    struct portal_buffer* pcb = calloc(1, sizeof(struct portal_buffer));
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
                spa_data->data = NULL;
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
            pcb = calloc(1, sizeof(struct portal_buffer));
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
    spa_data->data = NULL;
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
    struct portal_buffer* pcb = pw_buffer->user_data;
    if (pcb) {
        pw_buffer->user_data = NULL;
        portal_buffer_destroy(pcb);
    }
}

static void write_stream_meta(struct screencast_state* state, struct spa_buffer* spa_buffer)
{
    struct spa_meta_header* header = spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof(struct spa_meta_header));
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
        struct spa_meta_region* region = spa_meta_first(damage);
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
    struct screencast_state* state = data;
    struct pw_buffer* pw_buffer = pw_stream_dequeue_buffer(state->stream);
    if (!pw_buffer)
        return;
    struct spa_buffer* spa_buffer = pw_buffer->buffer;
    struct portal_buffer* pcb = pw_buffer->user_data;

    if (!pcb || !pcb->wl_buffer || !state->wayland_running) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast process without buffer (pcb=%p wl=%p running=%d)\n",
            (void*)pcb, pcb ? (void*)pcb->wl_buffer : NULL, state->wayland_running);
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
    state->pending_buffer = NULL;
    pthread_mutex_unlock(&state->capture_mutex);

    struct spa_data* spa_data = &spa_buffer->datas[0];
    if (status == 1) {
        spa_data->chunk->offset = 0;
        spa_data->chunk->size = spa_data->maxsize;
        spa_data->chunk->stride = pcb->stride;
        spa_data->chunk->flags = SPA_CHUNK_FLAG_NONE;
        write_stream_meta(state, spa_buffer);
        if (!state->produced_first_buffer) {
            state->produced_first_buffer = TRUE;
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
    struct screencast_state* s = data;
    if (error)
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream error: %s\n", error);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        int node = pw_stream_get_node_id(s->stream);
        if (node > 0)
            s->stream_node_id = node;
    }
}

static gboolean test_dmabuf_modifier(struct screencast_state* state, uint64_t modifier)
{
    if (!state->gbm || state->stream_width <= 0 || state->stream_height <= 0)
        return FALSE;
    uint32_t fourcc = state->capture_dmabuf_fourcc ? state->capture_dmabuf_fourcc : DRM_FORMAT_ARGB8888;
    uint64_t modifiers[1] = { modifier };
    struct gbm_bo* bo = gbm_bo_create_with_modifiers(state->gbm, (uint32_t)state->stream_width,
        (uint32_t)state->stream_height, fourcc, modifiers, 1);
    if (!bo)
        return FALSE;
    gbm_bo_destroy(bo);
    return TRUE;
}

static void on_stream_param_changed(void* data, uint32_t id, const struct spa_pod* param)
{
    struct screencast_state* state = data;
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
        // Dual modifier negotiation: pick one we can allocate and re-offer it
        // fixated (mirrors KWin's onStreamParamChanged). Only offer once; if
        // PipeWire still reports fixation-required, accept our choice.
        if (info.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED && !state->fixate_offered) {
            const struct spa_pod_prop* prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
            uint64_t chosen = state->capture_dmabuf_modifiers[0];
            if (prop) {
                uint32_t count = SPA_POD_CHOICE_N_VALUES(&prop->value);
                uint64_t const* values = SPA_POD_CHOICE_VALUES(&prop->value);
                for (uint32_t i = 0; i < count; ++i) {
                    if (test_dmabuf_modifier(state, values[i])) {
                        chosen = values[i];
                        break;
                    }
                }
            }
            state->fixate_offered = TRUE;
            state->fixate_modifier = chosen;
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast fixating DMA-BUF modifier=0x%llx\n", (unsigned long long)chosen);
            uint8_t buffer[4096];
            struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            const struct spa_pod* params[4];
            int n = build_enum_formats(state, &builder, params, TRUE, chosen, state->stream_width, state->stream_height);
            pw_stream_update_params(state->stream, params, n);
            return;
        }
        state->negotiated_dmabuf = state->gbm && state->capture_has_dmabuf;
        state->negotiated_modifier = state->negotiated_dmabuf
            ? ((info.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) ? state->fixate_modifier : info.modifier)
            : 0;
    } else {
        state->negotiated_dmabuf = FALSE;
        state->negotiated_modifier = 0;
    }
    state->negotiated = TRUE;

    fprintf(stderr, "xdg-desktop-portal-serenity: screencast negotiated %dx%d spa-format %d fourcc 0x%08x %s modifier=0x%llx\n",
        state->stream_width, state->stream_height, info.format, state->negotiated_fourcc,
        state->negotiated_dmabuf ? "DMA-BUF" : "memfd", (unsigned long long)state->negotiated_modifier);

    announce_stream_params(state);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
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
    if (serial && name && g_strcmp0(name, "sde-screencast") == 0) {
        guint64 value = g_ascii_strtoull(serial, NULL, 10);
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
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void on_core_done(void* data, uint32_t id, int seq)
{
    (void)id;
    (void)seq;
    ((struct screencast_state*)data)->core_sync_done = 1;
}
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
};

// Create and connect the PipeWire stream for `width`x`height` BGRA video. Returns
// the node id, or -1.
// Build the EnumFormat list: a DMA-BUF format (with the compositor's
// modifiers, either as a DONT_FIXATE choice or fixated to one) plus a memfd
// fallback. Mirrors KWin's buildFormats().
static int build_enum_formats(struct screencast_state* state, struct spa_pod_builder* b,
    const struct spa_pod** params, gboolean fixate, uint64_t fixate_modifier, int width, int height)
{
    int n = 0;
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)width, (uint32_t)height);
    struct spa_rectangle size_min = SPA_RECTANGLE(1, 1);
    struct spa_rectangle size_max = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction rate = SPA_FRACTION(30, 1);

    if (state->capture_has_dmabuf && state->dmabuf && state->gbm && state->dmabuf_enabled && state->capture_dmabuf_modifier_count > 0) {
        uint32_t spa_format = fourcc_to_spa_video_format(state->capture_dmabuf_fourcc);
        if (spa_format == SPA_VIDEO_FORMAT_UNKNOWN)
            spa_format = SPA_VIDEO_FORMAT_BGRA;
        // When fixating, offer the chosen modifier as a single fixated format
        // first, then the full DONT_FIXATE choice (as KWin does).
        if (fixate) {
            struct spa_pod_frame frame;
            spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
            spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&size), SPA_POD_Rectangle(&size_min), SPA_POD_Rectangle(&size_max)), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate), 0);
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(b, fixate_modifier);
            params[n++] = spa_pod_builder_pop(b, &frame);
        }
        struct spa_pod_frame frame;
        struct spa_pod_frame choice;
        {
            spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
            spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&size), SPA_POD_Rectangle(&size_min), SPA_POD_Rectangle(&size_max)), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate), 0);
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
            for (size_t i = 0; i < state->capture_dmabuf_modifier_count; ++i) {
                spa_pod_builder_long(b, state->capture_dmabuf_modifiers[i]);
                if (i == 0)
                    spa_pod_builder_long(b, state->capture_dmabuf_modifiers[i]);
            }
            spa_pod_builder_pop(b, &choice);
            params[n++] = spa_pod_builder_pop(b, &frame);
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
    params[n++] = spa_pod_builder_pop(b, &frame);
    return n;
}

static int start_pipewire_stream(int width, int height)
{
    if (!g_state.pw_loop) {
        pw_init(NULL, NULL);
        g_state.pw_loop = pw_thread_loop_new("sde-screencast", NULL);
        if (!g_state.pw_loop) {
            fprintf(stderr, "xdg-desktop-portal-serenity: pw_thread_loop_new failed\n");
            return -1;
        }
        g_state.pw_context = pw_context_new(pw_thread_loop_get_loop(g_state.pw_loop), NULL, 0);
        // Start the loop before connecting so the server can assign the node id.
        pw_thread_loop_start(g_state.pw_loop);
    }

    pw_thread_loop_lock(g_state.pw_loop);

    if (!g_state.pw_core && g_state.pw_context) {
        g_state.pw_core = pw_context_connect(g_state.pw_context, NULL, 0);
        if (g_state.pw_core) {
            pw_core_add_listener(g_state.pw_core, &g_state.core_listener, &core_events, &g_state);
            g_state.pw_registry = pw_core_get_registry(g_state.pw_core, PW_VERSION_REGISTRY, 0);
            if (g_state.pw_registry)
                pw_registry_add_listener(g_state.pw_registry, &g_state.registry_listener, &registry_events, &g_state);
        }
    }

    if (g_state.stream) {
        pw_stream_destroy(g_state.stream);
        g_state.stream = NULL;
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
            NULL));
    if (!g_state.stream) {
        fprintf(stderr, "xdg-desktop-portal-serenity: pw_stream_new failed\n");
        pw_thread_loop_unlock(g_state.pw_loop);
        return -1;
    }
    pw_stream_add_listener(g_state.stream, &g_state.stream_listener, &stream_events, &g_state);
    g_state.fixate_offered = FALSE;
    g_state.negotiated = FALSE;

    uint8_t buffer[4096];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[8];
    int param_count = build_enum_formats(&g_state, &builder, params, FALSE, 0, width, height);
    // Also offer the buffer layout and metadata up front, so the server can
    // negotiate them together with the format.
    {
        int stride = width * 4;
        int buffertypes = (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd);
        params[param_count++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes));
        params[param_count++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
        params[param_count++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)sizeof(struct spa_meta_region) * 16,
                (int)sizeof(struct spa_meta_region), (int)sizeof(struct spa_meta_region) * 16));
        params[param_count++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size, SPA_POD_Int((int)cursor_meta_size()));
    }

    int result = pw_stream_connect(g_state.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_ALLOC_BUFFERS, (const struct spa_pod**)params, param_count);
    if (result < 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: pw_stream_connect failed: %d\n", result);
        pw_stream_destroy(g_state.stream);
        g_state.stream = NULL;
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
    params[n++] = spa_pod_builder_pop(&builder, &frame);

    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)sizeof(struct spa_meta_region) * 16,
            (int)sizeof(struct spa_meta_region), (int)sizeof(struct spa_meta_region) * 16));
    if (state->cursor_metadata)
        params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
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
    struct spa_meta_cursor* cursor = spa_buffer_find_meta_data(spa_buffer, SPA_META_Cursor, sizeof(struct spa_meta_cursor));
    if (!cursor) {
        if (!state->cursor_meta_logged) {
            state->cursor_meta_logged = TRUE;
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
    state->cursor_bitmap_dirty = FALSE;
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
        state->capture_in_flight = FALSE;
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
    struct capture* capture = data;
    if (!capture->first_frame_logged) {
        capture->first_frame_logged = TRUE;
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast first frame %dx%d\n", capture->width, capture->height);
    }
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;
    complete_pending_capture(capture->state, 1);
}

static void frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    struct capture* capture = data;
    (void)reason;
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;
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
    struct capture* capture = data;
    capture->width = (int)width;
    capture->height = (int)height;
}
static void capture_session_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)session;
    struct capture* capture = data;
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
    uint64_t* modifier;
    wl_array_for_each(modifier, modifiers) {
        if (g_state.capture_dmabuf_modifier_count < 64)
            g_state.capture_dmabuf_modifiers[g_state.capture_dmabuf_modifier_count++] = *modifier;
    }
    g_state.capture_has_dmabuf = g_state.capture_dmabuf_modifier_count > 0;
}

static void setup_cursor_session(struct capture* capture);
static void cursor_capture_next(void);

static void capture_session_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    struct capture* capture = data;
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
    g_state.cursor_visible = TRUE;
    cursor_capture_next();
}
static void cursor_session_leave(void* data, struct ext_image_copy_capture_cursor_session_v1* session)
{
    (void)data;
    (void)session;
    g_state.cursor_visible = FALSE;
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
    g_state.cursor_frame = NULL;

    if (g_state.cursor_map && g_state.cursor_width > 0 && g_state.cursor_height > 0) {
        uint32_t fourcc = g_state.cursor_shm_format ? (uint32_t)g_state.cursor_shm_format : DRM_FORMAT_ARGB8888;
        uint32_t spa_format = fourcc_to_spa_video_format(fourcc);
        int stride = g_state.cursor_stride;
        size_t row = (size_t)g_state.cursor_width * 4;
        size_t rows = (size_t)g_state.cursor_height;
        uint8_t* dst = realloc(g_state.cursor_bitmap, row * rows);
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
            g_state.cursor_bitmap_valid = TRUE;
            g_state.cursor_bitmap_dirty = TRUE;
            (void)spa_format;
        }
    }

    if (g_state.cursor_map) {
        munmap(g_state.cursor_map, g_state.cursor_map_size);
        g_state.cursor_map = NULL;
    }
    if (g_state.cursor_buffer) {
        wl_buffer_destroy(g_state.cursor_buffer);
        g_state.cursor_buffer = NULL;
    }
}
static void cursor_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)data;
    (void)reason;
    ext_image_copy_capture_frame_v1_destroy(frame);
    g_state.cursor_frame = NULL;
    if (g_state.cursor_map) {
        munmap(g_state.cursor_map, g_state.cursor_map_size);
        g_state.cursor_map = NULL;
    }
    if (g_state.cursor_buffer) {
        wl_buffer_destroy(g_state.cursor_buffer);
        g_state.cursor_buffer = NULL;
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
    uint8_t* map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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
    ext_image_copy_capture_frame_v1_add_listener(g_state.cursor_frame, &cursor_frame_listener, NULL);
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
    g_state.cursor_metadata = TRUE;
    g_state.cursor_session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
        g_state.capture_manager, source, g_state.pointer);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_cursor_session_v1_add_listener(g_state.cursor_session, &cursor_session_listener, NULL);
    g_state.cursor_capture = ext_image_copy_capture_cursor_session_v1_get_capture_session(g_state.cursor_session);
    ext_image_copy_capture_session_v1_add_listener(g_state.cursor_capture, &cursor_capture_listener, NULL);
    fprintf(stderr, "xdg-desktop-portal-serenity: screencast metadata cursor session created\n");
    (void)capture;
}

static void ftl_handle_closed(void* data, struct ext_foreign_toplevel_handle_v1* handle)
{
    (void)handle;
    struct toplevel_info* toplevel = data;
    toplevel->handle = NULL;
}
static void ftl_handle_done(void* data, struct ext_foreign_toplevel_handle_v1* handle)
{
    (void)data;
    (void)handle;
}
static void ftl_handle_title(void* data, struct ext_foreign_toplevel_handle_v1* handle, char const* title)
{
    (void)handle;
    struct toplevel_info* toplevel = data;
    free(toplevel->title);
    toplevel->title = strdup(title);
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
    struct toplevel_info* toplevel = data;
    free(toplevel->identifier);
    toplevel->identifier = strdup(identifier);
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
    struct screencast_state* state = data;
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
        g_state.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g_state.dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !g_state.seat) {
        g_state.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(g_state.seat, &seat_listener, NULL);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g_state.source_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g_state.capture_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0)
        g_state.foreign_source_manager = wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        g_state.foreign_toplevel_list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(g_state.foreign_toplevel_list, &ftl_listener, &g_state);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && g_state.output_count < MAX_OUTPUTS) {
        g_state.outputs[g_state.output_count++] = wl_registry_bind(registry, name, &wl_output_interface, version);
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
    state->capture_in_flight = TRUE;
    pthread_mutex_unlock(&state->capture_mutex);

    capture->frame = ext_image_copy_capture_session_v1_create_frame(capture->session);
    ext_image_copy_capture_frame_v1_add_listener(capture->frame, &frame_listener, capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture->frame, pcb->wl_buffer);
    ext_image_copy_capture_frame_v1_capture(capture->frame);
}

static void* capture_thread(void* data)
{
    struct screencast_state* state = data;
    while (state->wayland_running) {
        begin_pending_capture(state);
        wl_display_dispatch_pending(state->display);
        while (wl_display_prepare_read(state->display) != 0) {
            if (wl_display_dispatch_pending(state->display) < 0)
                return NULL;
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
            return NULL;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (wl_display_read_events(state->display) < 0)
                return NULL;
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
    return NULL;
}

// Set up the Wayland connection (idempotent).
static gboolean ensure_wayland(struct screencast_state* state)
{
    if (state->display)
        return TRUE;

    pthread_mutex_init(&state->capture_mutex, NULL);
    pthread_cond_init(&state->capture_cond, NULL);
    if (pipe(state->wake_pipe) != 0)
        return FALSE;
    fcntl(state->wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(state->wake_pipe[1], F_SETFL, O_NONBLOCK);
    state->wayland_running = TRUE;

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
    // DMA-BUF capture is opt-in: this PipeWire build fails client-node buffer
    // allocation for the DMA-BUF path (docs/15 §6.2), so memfd is the default.
    state->dmabuf_enabled = getenv("SDE_SCREENCAST_DMABUF") != NULL;

    state->display = wl_display_connect(NULL);
    if (!state->display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast cannot connect to Wayland\n");
        return FALSE;
    }
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    wl_display_roundtrip(state->display); // globals
    wl_display_roundtrip(state->display); // seat capabilities
    wl_display_roundtrip(state->display); // output/toplevel info
    return TRUE;
}

// Run the LibGUI source chooser (SDE_PORTAL_CHOOSER) with the candidate labels;
// returns the chosen index, or 0 when there is no chooser.
static int run_source_chooser(char** labels, int count)
{
    char const* chooser = getenv("SDE_PORTAL_CHOOSER");
    if (!chooser || !*chooser || count <= 0)
        return 0;

    char** argv = calloc((size_t)count + 5, sizeof(char*));
    argv[0] = (char*)chooser;
    argv[1] = (char*)"-t";
    argv[2] = (char*)"Share your screen";
    for (int i = 0; i < count; ++i)
        argv[3 + i] = labels[i];
    argv[3 + count] = NULL;

    char* out = NULL;
    gint wait_status = 0;
    GError* error = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error);
    int chosen = 0;
    if (ok && out && g_spawn_check_wait_status(wait_status, NULL))
        chosen = atoi(out);
    if (out)
        g_free(out);
    if (error)
        g_error_free(error);
    free(argv);
    if (chosen < 0 || chosen >= count)
        chosen = 0;
    return chosen;
}

// Set up the capture for the requested source (the chosen window, else the
// primary output); returns the capture, or NULL.
static struct capture* begin_capture(struct screencast_state* state, struct session* session)
{
    if (!ensure_wayland(state))
        return NULL;
    if (!state->shm || !state->source_manager || !state->capture_manager || state->output_count == 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast missing capture globals (shm=%p sm=%p cm=%p outputs=%zu)\n",
            (void*)state->shm, (void*)state->source_manager, (void*)state->capture_manager, state->output_count);
        return NULL;
    }

    struct capture* capture = calloc(1, sizeof(struct capture));
    capture->state = state;
    capture->shm_format = WL_SHM_FORMAT_XRGB8888;
    capture->cursor_mode = session ? session->cursor_mode : 1;

    // Reset the DMA-BUF constraint info for this session.
    state->capture_has_dmabuf = FALSE;
    state->capture_dmabuf_modifier_count = 0;

    // A chosen window when the compositor offers it; otherwise the primary output.
    struct ext_image_capture_source_v1* source = NULL;
    if (session && session->chosen_toplevel_index >= 0 && state->foreign_source_manager) {
        struct toplevel_info* toplevel = &state->toplevels[session->chosen_toplevel_index];
        if (toplevel->handle) {
            source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(state->foreign_source_manager, toplevel->handle);
            capture->source_type = 2;
            capture->source_id = toplevel->identifier ? strdup(toplevel->identifier) : NULL;
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
        state->capture_thread_started = TRUE;
        pthread_create(&state->capture_thread, NULL, capture_thread, state);
    }
    wl_display_flush(state->display);
    return capture;
}

// --- D-Bus interface --------------------------------------------------------

const char portal_screencast_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.ScreenCast'>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SelectSources'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Start'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='AvailableSourceTypes'>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='AvailableCursorModes'>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static struct session* find_session(char const* handle)
{
    if (!g_state.sessions)
        g_state.sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return g_hash_table_lookup(g_state.sessions, handle);
}

static void return_response(GDBusMethodInvocation* invocation, guint32 response, GVariantBuilder* results)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, results));
}

// --- restore tokens ---------------------------------------------------------
//
// The XDG ScreenCast restore token lets an application remember a source so the
// user is not asked to pick again. Tokens are persisted as one pipe-separated
// record per line ("token|types|cursor_mode|source_id") in the SDE state
// directory.

static gchar* screencast_state_path(char const* name)
{
    char const* state = getenv("SDE_STATE_DIR");
    gchar* base;
    if (state && *state) {
        base = g_strdup(state);
    } else {
        char const* config = getenv("XDG_CONFIG_HOME");
        if (config && *config)
            base = g_build_filename(config, "sde", NULL);
        else
            base = g_build_filename(g_get_home_dir(), ".config", "sde", NULL);
    }
    g_mkdir_with_parents(base, 0700);
    gchar* path = g_build_filename(base, name, NULL);
    g_free(base);
    return path;
}

void portal_restore_token_store(char const* token, uint32_t types, uint32_t cursor_mode, char const* source_id)
{
    gchar* path = screencast_state_path("screencast-tokens");
    FILE* file = fopen(path, "a");
    g_free(path);
    if (!file)
        return;
    fprintf(file, "%s|%u|%u|%s\n", token, types, cursor_mode, source_id ? source_id : "");
    fclose(file);
}

gboolean portal_restore_token_load(char const* token, uint32_t* types, uint32_t* cursor_mode, char** source_id)
{
    gchar* path = screencast_state_path("screencast-tokens");
    gchar* contents = NULL;
    gboolean found = FALSE;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar** lines = g_strsplit(contents, "\n", -1);
        for (size_t i = 0; lines[i]; ++i) {
            char* line = lines[i];
            if (!*line)
                continue;
            char* bar = strchr(line, '|');
            if (!bar)
                continue;
            *bar = '\0';
            if (strcmp(line, token) != 0)
                continue;
            char* rest = bar + 1;
            char* bar2 = strchr(rest, '|');
            if (!bar2)
                continue;
            *bar2 = '\0';
            *types = (uint32_t)strtoul(rest, NULL, 10);
            char* rest2 = bar2 + 1;
            char* bar3 = strchr(rest2, '|');
            if (!bar3)
                continue;
            *bar3 = '\0';
            *cursor_mode = (uint32_t)strtoul(rest2, NULL, 10);
            if (source_id)
                *source_id = strdup(bar3 + 1);
            found = TRUE;
            break;
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    return found;
}

void portal_screencast_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    GVariantBuilder results;

    if (g_strcmp0(method_name, "CreateSession") == 0) {
        char const* handle = NULL;
        char const* session_handle = NULL;
        g_variant_get(parameters, "(&o&o&s@a{sv})", &handle, &session_handle, NULL, NULL);
        struct session* session = calloc(1, sizeof(struct session));
        session->handle = g_strdup(session_handle);
        session->types = 1; // monitor
        session->cursor_mode = 1;
        session->chosen_toplevel_index = -1;
        find_session(session_handle); // ensure the table exists
        g_hash_table_replace(g_state.sessions, g_strdup(session_handle), session);
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "SelectSources") == 0) {
        char const* session_handle = NULL;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&o&s@a{sv})", NULL, &session_handle, NULL, &options);
        struct session* session = find_session(session_handle);
        if (session && options) {
            g_variant_lookup(options, "types", "u", &session->types);
            g_variant_lookup(options, "cursor_mode", "u", &session->cursor_mode);
            g_variant_lookup(options, "persist_mode", "u", &session->persist_mode);

            // Restore a previously persisted source if the app supplies a token.
            char const* restore = NULL;
            g_variant_lookup(options, "restore_token", "&s", &restore);
            if (restore && *restore) {
                uint32_t types = session->types;
                uint32_t cursor_mode = session->cursor_mode;
                char* source_id = NULL;
                if (portal_restore_token_load(restore, &types, &cursor_mode, &source_id)) {
                    session->types = types;
                    session->cursor_mode = cursor_mode;
                    free(session->restore_token);
                    session->restore_token = strdup(restore);
                    if (source_id && *source_id) {
                        ensure_wayland(&g_state);
                        for (size_t i = 0; i < g_state.toplevel_count; ++i) {
                            if (g_state.toplevels[i].identifier && strcmp(g_state.toplevels[i].identifier, source_id) == 0) {
                                session->chosen_toplevel_index = (int)i;
                                break;
                            }
                        }
                    }
                }
                free(source_id);
                if (session->restore_token) {
                    if (options)
                        g_variant_unref(options);
                    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
                    g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(session->restore_token));
                    return_response(invocation, 0, &results);
                    return;
                }
            }
        }
        // When window sources are offered and a chooser is configured, let the
        // user pick via the LibGUI ScreenCastChooser (index 0 = entire screen).
        char const* chooser = getenv("SDE_PORTAL_CHOOSER");
        if (session && (session->types & 2) && chooser && *chooser) {
            ensure_wayland(&g_state);
            int count = 1 + (int)g_state.toplevel_count;
            char** labels = calloc((size_t)count, sizeof(char*));
            labels[0] = strdup("Entire screen");
            for (size_t i = 0; i < g_state.toplevel_count; ++i) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "Window: %s",
                    g_state.toplevels[i].title ? g_state.toplevels[i].title
                                               : (g_state.toplevels[i].identifier ? g_state.toplevels[i].identifier : "Window"));
                labels[1 + i] = strdup(buffer);
            }
            int chosen = run_source_chooser(labels, count);
            for (int i = 0; i < count; ++i)
                free(labels[i]);
            free(labels);
            session->chosen_toplevel_index = chosen - 1; // -1 => entire screen
        }

        // Mint and persist a restore token when the app asked to remember the
        // choice (persist_mode 1 = permanent, 2 = until revoked).
        if (session && session->persist_mode != 0 && !session->restore_token) {
            char token[33];
            for (int i = 0; i < 32; ++i)
                token[i] = "0123456789abcdef"[g_random_int_range(0, 16)];
            token[32] = '\0';
            char const* source_id = NULL;
            if (session->chosen_toplevel_index >= 0 && (size_t)session->chosen_toplevel_index < g_state.toplevel_count)
                source_id = g_state.toplevels[session->chosen_toplevel_index].identifier;
            portal_restore_token_store(token, session->types, session->cursor_mode, source_id);
            session->restore_token = strdup(token);
        }
        if (options)
            g_variant_unref(options);
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        if (session && session->restore_token)
            g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(session->restore_token));
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "Start") == 0) {
        char const* session_handle = NULL;
        g_variant_get(parameters, "(o&o&ss@a{sv})", NULL, &session_handle, NULL, NULL, NULL);
        struct session* session = find_session(session_handle);
        if (!session) {
            g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
            return_response(invocation, 2, &results);
            return;
        }
        session->capture = begin_capture(&g_state, session);
        if (!session->capture) {
            g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
            return_response(invocation, 2, &results);
            return;
        }
        // Wait for the stream (created when the capture session reports its size).
        for (int i = 0; i < 100 && g_state.stream_node_id <= 0; ++i) {
            wl_display_flush(g_state.display);
            usleep(10000);
        }
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        if (g_state.stream_node_id > 0) {
            GVariantBuilder streams;
            g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", 0, 0));
            g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", g_state.stream_width, g_state.stream_height));
            g_variant_builder_add(&props, "{sv}", "source_type", g_variant_new_uint32(session->capture->source_type));
            if (session->capture->source_id)
                g_variant_builder_add(&props, "{sv}", "id", g_variant_new_string(session->capture->source_id));
            if (g_state.stream_serial)
                g_variant_builder_add(&props, "{sv}", "pipewire-serial", g_variant_new_uint64(g_state.stream_serial));
            g_variant_builder_add(&streams, "(ua{sv})", (guint32)g_state.stream_node_id, &props);
            g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
            if (session->restore_token)
                g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(session->restore_token));
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream node %d (%dx%d, source_type %u, cursor %u)\n",
                g_state.stream_node_id, g_state.stream_width, g_state.stream_height,
                session->capture->source_type, session->capture->cursor_mode);
            return_response(invocation, 0, &results);
        } else {
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream not ready\n");
            return_response(invocation, 2, &results);
        }
        return;
    }

    if (g_strcmp0(method_name, "AvailableSourceTypes") == 0) {
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "types", g_variant_new_uint32(3)); // monitor | window
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "AvailableCursorModes") == 0) {
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "cursor_modes", g_variant_new_uint32(7)); // hidden | embedded | metadata
        return_response(invocation, 0, &results);
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

GVariant* portal_screencast_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(6);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
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
    struct screenshot_capture* shot = data;
    shot->width = (int)width;
    shot->height = (int)height;
}
static void shot_session_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)session;
    struct screenshot_capture* shot = data;
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
    struct screenshot_capture* shot = data;
    shot->status = 1;
    ext_image_copy_capture_frame_v1_destroy(frame);
    shot->frame = NULL;
}
static void shot_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)reason;
    struct screenshot_capture* shot = data;
    shot->status = -1;
    ext_image_copy_capture_frame_v1_destroy(frame);
    shot->frame = NULL;
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
    struct screenshot_capture* shot = data;
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
    shot->pixels = mmap(NULL, shot->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shot->pixels == MAP_FAILED) {
        shot->pixels = NULL;
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
    struct screenshot_capture* shot = data;
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
    struct shot_globals* globals = data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
        globals->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        globals->source_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        globals->capture_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0 && !globals->output)
        globals->output = wl_registry_bind(registry, name, &wl_output_interface, version);
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

int portal_screencast_capture_png(char const* path)
{
    // A dedicated Wayland connection, so a one-shot capture never races the
    // streaming capture thread on the shared display.
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screenshot cannot connect to Wayland\n");
        return -1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    struct shot_globals globals = { 0 };
    wl_registry_add_listener(registry, &shot_registry_listener, &globals);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!globals.shm || !globals.source_manager || !globals.capture_manager || !globals.output) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screenshot missing capture globals\n");
        wl_display_disconnect(display);
        return -1;
    }

    struct screenshot_capture shot = { 0 };
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
        GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, shot.width, shot.height);
        if (pixbuf) {
            guchar* dst = gdk_pixbuf_get_pixels(pixbuf);
            int dst_stride = gdk_pixbuf_get_rowstride(pixbuf);
            for (int y = 0; y < shot.height; ++y) {
                for (int x = 0; x < shot.width; ++x) {
                    uint8_t* src = shot.pixels + (size_t)y * shot.stride + (size_t)x * 4;
                    guchar* out = dst + (size_t)y * dst_stride + (size_t)x * 3;
                    out[0] = src[2]; // R (SHM is BGRA in memory)
                    out[1] = src[1]; // G
                    out[2] = src[0]; // B
                }
            }
            if (gdk_pixbuf_save(pixbuf, path, "png", NULL, NULL))
                result = 0;
            g_object_unref(pixbuf);
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

int portal_screencast_start_for_session(char const* session_handle, guint32* node_id, int* width, int* height, guint32* source_type)
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
    if (node_id)
        *node_id = (guint32)g_state.stream_node_id;
    if (width)
        *width = g_state.stream_width;
    if (height)
        *height = g_state.stream_height;
    if (source_type)
        *source_type = session->capture->source_type;
    return 0;
}
