/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 ScreenCast backend for xdg-desktop-portal-serenity.
//
// Captures an output with the *standard* ext-image-copy-capture-v1 protocol and
// publishes the frames as a PipeWire video stream, which the frontend hands to
// the requesting client. SHM capture (no DMA-BUF) keeps it portable; the stream
// is BGRA at the output's size.
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
    struct spa_hook stream_listener;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;

    // Latest captured frame, shared between the Wayland thread and the PipeWire
    // process callback.
    pthread_mutex_t frame_mutex;
    uint8_t* frame_data;
    int frame_width;
    int frame_height;
    gboolean frame_valid;
    gboolean produced_first_buffer;

    // Wayland
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_shm* shm;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* capture_manager;
    struct ext_foreign_toplevel_list_v1* foreign_toplevel_list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1* foreign_source_manager;
    struct wl_output* outputs[MAX_OUTPUTS];
    size_t output_count;
    struct toplevel_info toplevels[MAX_TOPLEVELS];
    size_t toplevel_count;

    pthread_t capture_thread;
    gboolean capture_thread_started;
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
    uint8_t* pixels;
    size_t mapped_size;
    int stride;
    struct wl_buffer* buffer;
    gboolean first_frame_logged;
    uint32_t source_type;
    char* source_id;
    uint32_t cursor_mode;
};

static struct screencast_state g_state = { 0 };

// --- PipeWire ---------------------------------------------------------------

static void update_frame_from_pixels(struct capture* capture, uint8_t const* pixels, int width, int height, int stride)
{
    pthread_mutex_lock(&g_state.frame_mutex);
    size_t size = (size_t)stride;
    if (!g_state.frame_data || g_state.frame_width != width || g_state.frame_height != height) {
        free(g_state.frame_data);
        g_state.frame_data = malloc((size_t)width * height * 4);
        g_state.frame_width = width;
        g_state.frame_height = height;
    }
    for (int y = 0; y < height; ++y)
        memcpy(g_state.frame_data + (size_t)y * width * 4, pixels + (size_t)y * stride, (size_t)width * 4);
    g_state.frame_valid = TRUE;
    pthread_mutex_unlock(&g_state.frame_mutex);
    (void)size;
}

static void on_stream_process(void* data)
{
    struct screencast_state* state = data;
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(state->stream);
    if (!buffer)
        return;
    struct spa_buffer* spa_buffer = buffer->buffer;
    if (!spa_buffer->datas[0].data) {
        pw_stream_queue_buffer(state->stream, buffer);
        return;
    }

    pthread_mutex_lock(&state->frame_mutex);
    if (state->frame_valid && state->frame_width > 0 && state->frame_height > 0) {
        int width = state->frame_width;
        int height = state->frame_height;
        size_t size = (size_t)width * height * 4;
        memcpy(spa_buffer->datas[0].data, state->frame_data, size);
        spa_buffer->datas[0].chunk->offset = 0;
        spa_buffer->datas[0].chunk->stride = width * 4;
        spa_buffer->datas[0].chunk->size = size;
        if (!state->produced_first_buffer) {
            state->produced_first_buffer = TRUE;
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast first pipewire buffer (%dx%d)\n", width, height);
        }
    } else {
        spa_buffer->datas[0].chunk->size = 0;
    }
    pthread_mutex_unlock(&state->frame_mutex);

    pw_stream_queue_buffer(state->stream, buffer);
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

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
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

    uint8_t buffer[1032];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame frame;
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)width, (uint32_t)height);
    struct spa_fraction rate = SPA_FRACTION(30, 1);

    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA), 0);
    spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate), 0);
    struct spa_pod* params[3] = { spa_pod_builder_pop(&builder, &frame) };

    // Buffer requirements (niri-style): memfd-backed, a handful of buffers.
    struct spa_pod_frame buffers_frame;
    spa_pod_builder_push_object(&builder, &buffers_frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16), 0);
    spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), 0);
    spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)), 0);
    params[1] = spa_pod_builder_pop(&builder, &buffers_frame);

    // The header meta (timestamps/flags), as niri advertises.
    struct spa_pod_frame meta_frame;
    spa_pod_builder_push_object(&builder, &meta_frame, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
    spa_pod_builder_add(&builder, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), 0);
    spa_pod_builder_add(&builder, SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)), 0);
    params[2] = spa_pod_builder_pop(&builder, &meta_frame);

    int result = pw_stream_connect(g_state.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, (const struct spa_pod**)params, 3);
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

// --- Wayland capture --------------------------------------------------------

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

static void start_frame(struct capture* capture);

static void frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    struct capture* capture = data;

    if (capture->pixels && capture->width > 0 && capture->height > 0) {
        update_frame_from_pixels(capture, capture->pixels, capture->width, capture->height, capture->stride);
        if (!capture->first_frame_logged) {
            capture->first_frame_logged = TRUE;
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast first frame %dx%d\n", capture->width, capture->height);
        }
    }

    if (capture->buffer) {
        wl_buffer_destroy(capture->buffer);
        capture->buffer = NULL;
    }
    if (capture->pixels) {
        munmap(capture->pixels, capture->mapped_size);
        capture->pixels = NULL;
    }

    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;

    usleep(33000); // ~30 fps
    start_frame(capture);
}

static void frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)reason;
    struct capture* capture = data;
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;
    if (capture->buffer) {
        wl_buffer_destroy(capture->buffer);
        capture->buffer = NULL;
    }
    if (capture->pixels) {
        munmap(capture->pixels, capture->mapped_size);
        capture->pixels = NULL;
    }
    start_frame(capture);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void start_frame(struct capture* capture)
{
    int stride = capture->width * 4;
    size_t size = (size_t)stride * capture->height;

    int fd = memfd_create("sde-screencast", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return;
    }
    uint8_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(g_state.shm, fd, (int)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, capture->width, capture->height, stride, capture->shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    capture->pixels = pixels;
    capture->mapped_size = size;
    capture->stride = stride;
    capture->buffer = buffer;

    capture->frame = ext_image_copy_capture_session_v1_create_frame(capture->session);
    ext_image_copy_capture_frame_v1_add_listener(capture->frame, &frame_listener, capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture->frame, buffer);
    ext_image_copy_capture_frame_v1_capture(capture->frame);
}

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
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}
static void capture_session_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)session;
    struct capture* capture = data;
    if (capture->width <= 0 || capture->height <= 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast session has no size\n");
        return;
    }
    // Publish the PipeWire stream now that the size is known.
    if (g_state.stream_node_id <= 0)
        start_pipewire_stream(capture->width, capture->height);
    start_frame(capture);
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

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
        g_state.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
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

static void* capture_thread(void* data)
{
    (void)data;
    while (wl_display_dispatch(g_state.display) >= 0) {
    }
    return NULL;
}

// Set up the Wayland connection (idempotent).
static gboolean ensure_wayland(struct screencast_state* state)
{
    if (state->display)
        return TRUE;
    state->display = wl_display_connect(NULL);
    if (!state->display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast cannot connect to Wayland\n");
        return FALSE;
    }
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    wl_display_roundtrip(state->display); // globals
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
    // capture (niri's "embedded" mode).
    uint32_t options = (capture->cursor_mode == 2) ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    capture->session = ext_image_copy_capture_manager_v1_create_session(state->capture_manager, source, options);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(capture->session, &capture_session_listener, capture);

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
        g_variant_builder_add(&results, "{sv}", "cursor_modes", g_variant_new_uint32(3)); // hidden | embedded
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
