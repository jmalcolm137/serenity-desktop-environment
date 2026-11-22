/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 ScreenCast infrastructure probe.
//
// A tiny Wayland client that answers "what can this compositor actually offer
// the ScreenCast portal?". It:
//
//   1. creates an output capture session (ext-image-copy-capture-v1) and
//      reports whether the source advertises DMA-BUF buffers (zero-copy),
//      which formats/modifiers, or SHM only;
//   2. captures one real frame into a DMA-BUF it allocates with GBM and imports
//      through zwp_linux_dmabuf_v1, proving the compositor renders into an
//      imported buffer (not just that it advertises the formats);
//   3. creates a pointer-cursor session
//      (ext_image_copy_capture_cursor_session_v1) and captures a cursor bitmap
//      frame, which is what the metadata cursor mode needs.
//
// Run it against a live SDE session on a real TTY (docs/15 §6.1) to see if the
// DMA-BUF / metadata-cursor work is unblocked. PROBE lines carry the results.
// Exit status is 0 only when a real output frame was captured.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

// gbm.h does not pull in drm_fourcc.h, and this is the one constant the probe needs.
// INVALID is what a compositor advertises to mean "implicit modifier"; GBM rejects it
// as a modifier argument, so the probe must allocate without one in that case.
#ifndef DRM_FORMAT_MOD_INVALID
#    define DRM_FORMAT_MOD_INVALID (((1ULL << 56) - 1))
#endif

#define MAX_FORMATS 8
#define MAX_MODIFIERS 64

struct dmabuf_format {
    uint32_t fourcc;
    uint64_t modifiers[MAX_MODIFIERS];
    size_t modifier_count;
};

struct probe {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_output* output;
    struct wl_shm* shm;
    struct zwp_linux_dmabuf_v1* dmabuf;
    struct ext_output_image_capture_source_manager_v1* source_manager;
    struct ext_image_copy_capture_manager_v1* capture_manager;

    struct gbm_device* gbm;

    // Output session constraints.
    int output_shm_formats;
    struct dmabuf_format output_formats[MAX_FORMATS];
    size_t output_format_count;
    int output_width;
    int output_height;
    int output_done;

    // Output frame capture result.
    int out_frame_ready;
    int out_frame_failed;
    int out_frame_used_dmabuf;
    uint64_t out_frame_checksum;

    // Cursor session.
    int cursor_session_ok;
    int cursor_enter;
    int cursor_position_seen;
    int cursor_width;
    int cursor_height;
    int cursor_frame_ready;
};

static struct probe g = { 0 };

// --- a one-shot capture into an shm buffer (for the cursor bitmap) ----------

struct shm_capture {
    int width;
    int height;
    int stride;
    int shm_format;
    uint8_t* data;
    size_t size;
    int ready;
};

static void out_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)data;
    (void)session;
    g.output_width = (int)width;
    g.output_height = (int)height;
}
static void out_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)data;
    (void)session;
    (void)format;
    g.output_shm_formats++;
}
static void out_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}
static void out_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)data;
    (void)session;
    if (g.output_format_count >= MAX_FORMATS)
        return;
    struct dmabuf_format* f = &g.output_formats[g.output_format_count++];
    f->fourcc = format;
    uint64_t* m;
    wl_array_for_each(m, modifiers) {
        if (f->modifier_count < MAX_MODIFIERS)
            f->modifiers[f->modifier_count++] = *m;
    }
    printf("PROBE output.dmabuf_format fourcc=0x%08x modifiers=%zu\n", format, f->modifier_count);
}

static void start_output_dmabuf_capture(void);

static void out_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
    g.output_done = 1;
    printf("PROBE output.session size=%dx%d shm_formats=%d dmabuf_formats=%zu\n",
        g.output_width, g.output_height, g.output_shm_formats, g.output_format_count);
    start_output_dmabuf_capture();
}
static void out_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
}
static const struct ext_image_copy_capture_session_v1_listener out_listener = {
    .buffer_size = out_buffer_size,
    .shm_format = out_shm_format,
    .dmabuf_device = out_dmabuf_device,
    .dmabuf_format = out_dmabuf_format,
    .done = out_done,
    .stopped = out_stopped,
};

static struct ext_image_copy_capture_session_v1* g_out_session;

// --- output DMA-BUF frame ---------------------------------------------------

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

static void out_frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    (void)data;
    (void)frame;
    g.out_frame_ready = 1;
}
static void out_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)data;
    (void)frame;
    g.out_frame_failed = 1;
    fprintf(stderr, "capture-probe: output DMA-BUF frame failed, reason=%u\n", reason);
}
static const struct ext_image_copy_capture_frame_v1_listener out_frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = out_frame_ready,
    .failed = out_frame_failed,
};

static void start_output_dmabuf_capture(void)
{
    if (g.output_format_count == 0 || !g.dmabuf || !g.gbm)
        return;
    if (g.output_width <= 0 || g.output_height <= 0)
        return;

    struct dmabuf_format* f = &g.output_formats[0];

    // Prefer a linear modifier, so the probe can map and verify the pixels; then a
    // concrete modifier over DRM_FORMAT_MOD_INVALID. The compositor advertises
    // INVALID to mean "implicit modifier", and GBM rejects it as a modifier argument,
    // which is how the first hardware run failed:
    //
    //   gbm_bo_create_with_modifiers(0x34325258, mod=0xffffffffffffff) failed: Invalid argument
    //
    // On the NVIDIA driver INVALID is simply the first entry, so picking [0] chose it.
    uint64_t chosen = DRM_FORMAT_MOD_INVALID;
    for (size_t i = 0; i < f->modifier_count; ++i) {
        if (f->modifiers[i] == 0) {
            chosen = 0;
            break;
        }
    }
    if (chosen == DRM_FORMAT_MOD_INVALID) {
        for (size_t i = 0; i < f->modifier_count; ++i) {
            if (f->modifiers[i] != DRM_FORMAT_MOD_INVALID) {
                chosen = f->modifiers[i];
                break;
            }
        }
    }

    // Only INVALID advertised (or none at all): allocate without a modifier and let
    // the driver choose. gbm_bo_get_modifier() then reports what it actually picked.
    bool const implicit_modifier = chosen == DRM_FORMAT_MOD_INVALID;
    struct gbm_bo* bo = implicit_modifier
        ? gbm_bo_create(g.gbm, (uint32_t)g.output_width, (uint32_t)g.output_height, f->fourcc,
              GBM_BO_USE_RENDERING)
        : gbm_bo_create_with_modifiers(g.gbm, (uint32_t)g.output_width,
              (uint32_t)g.output_height, f->fourcc, &chosen, 1);
    if (!bo) {
        fprintf(stderr, "capture-probe: gbm_bo_create(0x%08x, mod=0x%llx%s) failed: %s\n",
            f->fourcc, (unsigned long long)chosen, implicit_modifier ? ", implicit" : "", strerror(errno));
        return;
    }
    if (implicit_modifier)
        chosen = gbm_bo_get_modifier(bo);

    int fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        gbm_bo_destroy(bo);
        return;
    }

    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, gbm_bo_get_offset(bo, 0), gbm_bo_get_stride(bo),
        (uint32_t)(chosen >> 32), (uint32_t)(chosen & 0xffffffffu));
    struct wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params,
        g.output_width, g.output_height, gbm_bo_get_format(bo), 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!buffer) {
        close(fd);
        gbm_bo_destroy(bo);
        return;
    }

    struct ext_image_copy_capture_frame_v1* frame = ext_image_copy_capture_session_v1_create_frame(g_out_session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &out_frame_listener, NULL);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_capture(frame);

    printf("PROBE output.dmabuf_capture started fourcc=0x%08x modifier=0x%llx size=%dx%d stride=%u\n",
        f->fourcc, (unsigned long long)chosen, g.output_width, g.output_height, gbm_bo_get_stride(bo));

    g.out_frame_used_dmabuf = 1;

    // Wait for ready/failed and then checksum the rendered pixels.
    struct pollfd pfd = { .fd = wl_display_get_fd(g.display), .events = POLLIN };
    for (int i = 0; i < 60 && !g.out_frame_ready && !g.out_frame_failed; ++i) {
        wl_display_flush(g.display);
        if (poll(&pfd, 1, 50) > 0)
            wl_display_dispatch(g.display);
    }
    printf("PROBE output.dmabuf_capture ready=%d failed=%d\n", g.out_frame_ready, g.out_frame_failed);

    if (g.out_frame_ready) {
        uint32_t stride = 0;
        void* map_data = NULL;
        void* pixels = gbm_bo_map(bo, 0, 0, (uint32_t)g.output_width, (uint32_t)g.output_height,
            GBM_BO_TRANSFER_READ, &stride, &map_data);
        if (pixels) {
            uint64_t sum = 0;
            uint8_t* p = pixels;
            for (int y = 0; y < g.output_height; y += 8)
                for (int x = 0; x < g.output_width; x += 8)
                    sum += p[(size_t)y * stride + (size_t)x * 4] + p[(size_t)y * stride + (size_t)x * 4 + 1];
            g.out_frame_checksum = sum;
            gbm_bo_unmap(bo, map_data);
            printf("PROBE output.dmabuf_capture checksum=%llu\n", (unsigned long long)sum);
        } else {
            printf("PROBE output.dmabuf_capture mapped=no (linear mapping unavailable)\n");
        }
    }

    wl_buffer_destroy(buffer);
    close(fd);
    gbm_bo_destroy(bo);
}

// --- pointer cursor session -------------------------------------------------

struct cursor_capture {
    struct shm_capture shm;
    struct ext_image_copy_capture_session_v1* session;
};

static struct cursor_capture g_cursor = { 0 };

static void cursor_start_capture(void);

static void cur_buffer_size(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height)
{
    (void)data;
    (void)session;
    g_cursor.shm.width = (int)width;
    g_cursor.shm.height = (int)height;
    g.cursor_width = (int)width;
    g.cursor_height = (int)height;
}
static void cur_shm_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format)
{
    (void)data;
    (void)session;
    if (g_cursor.shm.shm_format == 0)
        g_cursor.shm.shm_format = (int)format;
}
static void cur_dmabuf_device(void* data, struct ext_image_copy_capture_session_v1* session, struct wl_array* device)
{
    (void)data;
    (void)session;
    (void)device;
}
static void cur_dmabuf_format(void* data, struct ext_image_copy_capture_session_v1* session, uint32_t format, struct wl_array* modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}
static void cur_done(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
    g.cursor_session_ok = 1;
    printf("PROBE cursor.session ready %dx%d\n", g_cursor.shm.width, g_cursor.shm.height);
    cursor_start_capture();
}
static void cur_stopped(void* data, struct ext_image_copy_capture_session_v1* session)
{
    (void)data;
    (void)session;
}
static const struct ext_image_copy_capture_session_v1_listener cur_capture_listener = {
    .buffer_size = cur_buffer_size,
    .shm_format = cur_shm_format,
    .dmabuf_device = cur_dmabuf_device,
    .dmabuf_format = cur_dmabuf_format,
    .done = cur_done,
    .stopped = cur_stopped,
};

static void cur_frame_ready(void* data, struct ext_image_copy_capture_frame_v1* frame)
{
    (void)data;
    (void)frame;
    g.cursor_frame_ready = 1;
}
static void cur_frame_failed(void* data, struct ext_image_copy_capture_frame_v1* frame, uint32_t reason)
{
    (void)data;
    (void)frame;
    fprintf(stderr, "capture-probe: cursor frame failed, reason=%u\n", reason);
}
static const struct ext_image_copy_capture_frame_v1_listener cur_frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = cur_frame_ready,
    .failed = cur_frame_failed,
};

static void cursor_start_capture(void)
{
    if (!g.shm || g_cursor.shm.width <= 0 || g_cursor.shm.height <= 0)
        return;
    int stride = g_cursor.shm.width * 4;
    size_t size = (size_t)stride * g_cursor.shm.height;
    int fd = memfd_create("capture-probe-cursor", MFD_CLOEXEC);
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
    struct wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, (int)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, g_cursor.shm.width, g_cursor.shm.height,
        stride, g_cursor.shm.shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    g_cursor.shm.data = pixels;
    g_cursor.shm.size = size;
    g_cursor.shm.stride = stride;

    struct ext_image_copy_capture_frame_v1* frame = ext_image_copy_capture_session_v1_create_frame(g_cursor.session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &cur_frame_listener, NULL);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    wl_display_flush(g.display);
}

static void cursor_enter(void* data, struct ext_image_copy_capture_cursor_session_v1* session)
{
    (void)data;
    (void)session;
    g.cursor_enter = 1;
    printf("PROBE cursor.enter\n");
}
static void cursor_leave(void* data, struct ext_image_copy_capture_cursor_session_v1* session)
{
    (void)data;
    (void)session;
    printf("PROBE cursor.leave\n");
}
static void cursor_position(void* data, struct ext_image_copy_capture_cursor_session_v1* session, int32_t x, int32_t y)
{
    (void)data;
    (void)session;
    g.cursor_position_seen = 1;
    printf("PROBE cursor.position x=%d y=%d\n", x, y);
}
static void cursor_hotspot(void* data, struct ext_image_copy_capture_cursor_session_v1* session, int32_t x, int32_t y)
{
    (void)data;
    (void)session;
    printf("PROBE cursor.hotspot x=%d y=%d\n", x, y);
}
static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
    .enter = cursor_enter,
    .leave = cursor_leave,
    .position = cursor_position,
    .hotspot = cursor_hotspot,
};

// --- registry ---------------------------------------------------------------

static void seat_capabilities(void* data, struct wl_seat* seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer)
        g.pointer = wl_seat_get_pointer(seat);
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
        g.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g.dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4);
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        g.source_manager = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        g.capture_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0 && !g.output)
        g.output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !g.seat) {
        g.seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(g.seat, &seat_listener, NULL);
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

static void drain(int roundtrips)
{
    for (int i = 0; i < roundtrips; ++i)
        wl_display_roundtrip(g.display);
}

int main(void)
{
    g.display = wl_display_connect(NULL);
    if (!g.display) {
        fprintf(stderr, "capture-probe: cannot connect to Wayland (WAYLAND_DISPLAY unset?)\n");
        return 2;
    }
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    drain(3); // globals + seat capabilities

    if (!g.source_manager || !g.capture_manager || !g.output) {
        fprintf(stderr, "capture-probe: missing globals (source_manager=%p capture_manager=%p output=%p)\n",
            (void*)g.source_manager, (void*)g.capture_manager, (void*)g.output);
        return 2;
    }

    int gbm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (gbm_fd < 0) {
        fprintf(stderr, "capture-probe: cannot open /dev/dri/renderD128: %s (DMA-BUF capture not testable)\n", strerror(errno));
    } else {
        g.gbm = gbm_create_device(gbm_fd);
    }

    struct ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(g.source_manager, g.output);

    g_out_session = ext_image_copy_capture_manager_v1_create_session(g.capture_manager, source, 0);
    ext_image_copy_capture_session_v1_add_listener(g_out_session, &out_listener, NULL);

    if (g.pointer) {
        struct ext_image_copy_capture_cursor_session_v1* cursor_session =
            ext_image_copy_capture_manager_v1_create_pointer_cursor_session(g.capture_manager, source, g.pointer);
        ext_image_copy_capture_cursor_session_v1_add_listener(cursor_session, &cursor_listener, NULL);
        g_cursor.session = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor_session);
        ext_image_copy_capture_session_v1_add_listener(g_cursor.session, &cur_capture_listener, NULL);
    } else {
        printf("PROBE cursor.session unavailable (no wl_pointer)\n");
    }

    // Dispatch until the frames have landed (or a timeout).
    for (int i = 0; i < 40; ++i) {
        wl_display_flush(g.display);
        struct pollfd pfd = { .fd = wl_display_get_fd(g.display), .events = POLLIN };
        if (poll(&pfd, 1, 100) > 0)
            wl_display_dispatch(g.display);
        if (g.out_frame_ready || g.out_frame_failed)
            if (g.cursor_frame_ready || !g.cursor_session_ok)
                break;
    }

    printf("PROBE summary output_dmabuf=%zu cursor_session=%d cursor_enter=%d cursor_position=%d "
           "out_frame_dmabuf=%d out_frame_ready=%d cursor_frame_ready=%d\n",
        g.output_format_count, g.cursor_session_ok, g.cursor_enter, g.cursor_position_seen,
        g.out_frame_used_dmabuf, g.out_frame_ready, g.cursor_frame_ready);

    if (g.output_format_count > 0)
        printf("PROBE verdict: compositor offers DMA-BUF capture formats (zero-copy possible)\n");
    else
        printf("PROBE verdict: SHM only — no DMA-BUF offered (renderer likely pixman/headless)\n");
    if (g.out_frame_ready)
        printf("PROBE verdict: captured a real frame into an imported DMA-BUF\n");
    else if (g.out_frame_used_dmabuf)
        printf("PROBE verdict: DMA-BUF capture FAILED (frame not ready)\n");
    if (g.cursor_session_ok)
        printf("PROBE verdict: pointer cursor session works (metadata cursor mode possible)\n");
    else
        printf("PROBE verdict: pointer cursor session not available\n");
    if (g.cursor_frame_ready)
        printf("PROBE verdict: captured a cursor bitmap frame (%dx%d)\n", g.cursor_width, g.cursor_height);

    (void)source;
    return (g.out_frame_ready) ? 0 : 1;
}
