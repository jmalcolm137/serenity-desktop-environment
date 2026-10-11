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

#define MAX_OUTPUTS 8

// --- extension interface names (matched in the registry) --------------------

// --- state ------------------------------------------------------------------

struct screencast_state {
    GHashTable* sessions; // session handle -> Session*

    // PipeWire
    struct pw_thread_loop* pw_loop;
    struct pw_stream* stream;
    int stream_width;
    int stream_height;
    int stream_node_id;

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
    struct wl_output* outputs[MAX_OUTPUTS];
    size_t output_count;

    pthread_t capture_thread;
    gboolean capture_thread_started;
};

struct session {
    char* handle;
    uint32_t types;
    uint32_t cursor_mode;
    gboolean started;
    struct capture* capture;
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
        // Start the loop before connecting so the server can assign the node id.
        pw_thread_loop_start(g_state.pw_loop);
    }

    pw_thread_loop_lock(g_state.pw_loop);

    if (g_state.stream) {
        pw_stream_destroy(g_state.stream);
        g_state.stream = NULL;
    }

    g_state.stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_state.pw_loop),
        "SDE ScreenCast",
        pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_NODE_NAME, "sde-screencast",
            PW_KEY_NODE_DESCRIPTION, "SDE ScreenCast",
            PW_KEY_NODE_VIRTUAL, "false",
            NULL),
        &stream_events, &g_state);
    if (!g_state.stream) {
        fprintf(stderr, "xdg-desktop-portal-serenity: pw_stream_new_simple failed\n");
        pw_thread_loop_unlock(g_state.pw_loop);
        return -1;
    }

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
    pw_thread_loop_unlock(g_state.pw_loop);

    // The node id is assigned asynchronously; wait for the stream to connect.
    for (int i = 0; i < 200 && g_state.stream_node_id <= 0; ++i)
        usleep(5000);
    if (g_state.stream_node_id <= 0) {
        pw_thread_loop_lock(g_state.pw_loop);
        g_state.stream_node_id = pw_stream_get_node_id(g_state.stream);
        pw_thread_loop_unlock(g_state.pw_loop);
    }

    fprintf(stderr, "xdg-desktop-portal-serenity: screencast pipewire node id %d (%dx%d)\n",
        g_state.stream_node_id, width, height);
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

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
        g_state.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g_state.source_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g_state.capture_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0 && g_state.output_count < MAX_OUTPUTS)
        g_state.outputs[g_state.output_count++] = wl_registry_bind(registry, name, &wl_output_interface, version);
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

// Set up the capture for the first output; returns the capture, or NULL.
static struct capture* begin_capture(struct screencast_state* state)
{
    if (!state->display) {
        state->display = wl_display_connect(NULL);
        if (!state->display) {
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast cannot connect to Wayland\n");
            return NULL;
        }
        state->registry = wl_display_get_registry(state->display);
        wl_registry_add_listener(state->registry, &registry_listener, state);
        wl_display_roundtrip(state->display); // globals
        wl_display_roundtrip(state->display); // output info
    }
    if (!state->shm || !state->source_manager || !state->capture_manager || state->output_count == 0) {
        fprintf(stderr, "xdg-desktop-portal-serenity: screencast missing capture globals (shm=%p sm=%p cm=%p outputs=%zu)\n",
            (void*)state->shm, (void*)state->source_manager, (void*)state->capture_manager, state->output_count);
        return NULL;
    }

    struct capture* capture = calloc(1, sizeof(struct capture));
    capture->state = state;
    capture->shm_format = WL_SHM_FORMAT_XRGB8888;

    struct ext_image_capture_source_v1* source = ext_output_image_capture_source_manager_v1_create_source(state->source_manager, state->outputs[0]);
    capture->session = ext_image_copy_capture_manager_v1_create_session(state->capture_manager, source, 0);
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
        }
        if (options)
            g_variant_unref(options);
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
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
        session->capture = begin_capture(&g_state);
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
            g_variant_builder_add(&props, "{sv}", "source_type", g_variant_new_uint32(1));
            g_variant_builder_add(&streams, "(ua{sv})", (guint32)g_state.stream_node_id, &props);
            g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream node %d (%dx%d)\n",
                g_state.stream_node_id, g_state.stream_width, g_state.stream_height);
            return_response(invocation, 0, &results);
        } else {
            fprintf(stderr, "xdg-desktop-portal-serenity: screencast stream not ready\n");
            return_response(invocation, 2, &results);
        }
        return;
    }

    if (g_strcmp0(method_name, "AvailableSourceTypes") == 0) {
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "types", g_variant_new_uint32(1)); // monitor
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "AvailableCursorModes") == 0) {
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "cursor_modes", g_variant_new_uint32(1)); // hidden
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
        return g_variant_new_uint32(4);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}
