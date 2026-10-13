// Minimal screen-capture client: captures the first output via the standard
// wlr-screencopy protocol (the one `grim` speaks) and writes a binary PPM.
//
// This is the small, general capture helper for headless screenshots (see
// docs/03-engineering-conventions.md D7). On a normal session, `grim` works
// directly; this exists so the same capture path is scriptable with no extra
// tools. Usage: sde-screenshot <out.ppm>
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm* s_shm = NULL;
static struct wl_output* s_output = NULL;
static struct zwlr_screencopy_manager_v1* s_manager = NULL;
static struct wl_buffer* s_buffer = NULL;
static void* s_data = NULL;
static int s_width = 0;
static int s_height = 0;
static uint32_t s_format = 0;
static uint32_t s_stride = 0;
static int s_done = 0;
static char const* s_out_path = NULL;

static void buffer_handle_buffer(void* data, struct zwlr_screencopy_frame_v1* frame, uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)data;
    s_width = (int)width;
    s_height = (int)height;
    s_format = format;
    s_stride = stride;

    if (getenv("SDE_SCREENSHOT_DEBUG"))
        fprintf(stderr, "sde-screenshot: buffer format=0x%08x (%c%c%c%c) %ux%u stride=%u bpp=%u\n",
            format, (char)(format & 0xff), (char)((format >> 8) & 0xff),
            (char)((format >> 16) & 0xff), (char)((format >> 24) & 0xff),
            width, height, stride, width ? stride / width : 0);

    int const size = (int)stride * s_height;
    char name[] = "/tmp/screencopy-XXXXXX";
    int fd = mkstemp(name);
    unlink(name);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "sde-screenshot: cannot create shm buffer\n");
        exit(1);
    }
    s_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s_data == MAP_FAILED) {
        fprintf(stderr, "sde-screenshot: mmap failed\n");
        exit(1);
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(s_shm, fd, size);
    s_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, (int)stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    zwlr_screencopy_frame_v1_copy(frame, s_buffer);
}

static void buffer_handle_flags(void*, struct zwlr_screencopy_frame_v1*, uint32_t) { }
static void buffer_handle_ready(void*, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    FILE* f = fopen(s_out_path, "wb");
    if (f) {
        // wl_shm formats differ in both channel order and pixel size. The
        // compositor may hand back a 32-bit format (XRGB/ARGB = B,G,R,X in
        // memory; XBGR/ABGR = R,G,B,X), a 24-bit one (BGR888/RGB888), or 16-bit
        // RGB565 — and the row stride can exceed width * bytes-per-pixel. So
        // honour the advertised format for both, rather than assuming 32-bit
        // XRGB (which both swapped channels and ran off the end of the mapping
        // for 24-bit buffers, crashing).
        unsigned bpp = 4;
        bool bgr = true;
        bool rgb565 = false;
        switch (s_format) {
        case WL_SHM_FORMAT_XRGB8888:
        case WL_SHM_FORMAT_ARGB8888:
            bpp = 4;
            bgr = true;
            break;
        case WL_SHM_FORMAT_XBGR8888:
        case WL_SHM_FORMAT_ABGR8888:
            bpp = 4;
            bgr = false;
            break;
        case WL_SHM_FORMAT_BGR888:
            // "[23:0] B:G:R little endian" -> R,G,B in memory.
            bpp = 3;
            bgr = false;
            break;
        case WL_SHM_FORMAT_RGB888:
            // "[23:0] R:G:B little endian" -> B,G,R in memory.
            bpp = 3;
            bgr = true;
            break;
        case WL_SHM_FORMAT_RGB565:
            bpp = 2;
            rgb565 = true;
            break;
        default:
            // Unknown format: trust the stride to stay in bounds.
            bpp = s_width > 0 && s_stride >= (uint32_t)s_width * 3 ? s_stride / (uint32_t)s_width : 4;
            bgr = true;
            break;
        }
        fprintf(f, "P6\n%d %d\n255\n", s_width, s_height);
        for (int y = 0; y < s_height; ++y) {
            uint8_t const* row = (uint8_t const*)s_data + (size_t)y * s_stride;
            for (int x = 0; x < s_width; ++x) {
                uint8_t const* p = &row[(size_t)x * bpp];
                if (rgb565) {
                    uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                    fputc(((v >> 11) & 0x1f) * 255 / 31, f);
                    fputc(((v >> 5) & 0x3f) * 255 / 63, f);
                    fputc((v & 0x1f) * 255 / 31, f);
                } else if (bgr) {
                    fputc(p[2], f);
                    fputc(p[1], f);
                    fputc(p[0], f);
                } else {
                    fputc(p[0], f);
                    fputc(p[1], f);
                    fputc(p[2], f);
                }
            }
        }
        fclose(f);
    }
    s_done = 1;
    zwlr_screencopy_frame_v1_destroy(frame);
}
static void buffer_handle_failed(void*, struct zwlr_screencopy_frame_v1* frame)
{
    zwlr_screencopy_frame_v1_destroy(frame);
    s_done = 1;
}
static void buffer_handle_damage(void*, struct zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t, uint32_t) { }
static void buffer_handle_linux_dmabuf(void*, struct zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) { }
static void buffer_handle_buffer_done(void*, struct zwlr_screencopy_frame_v1*) { }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = buffer_handle_buffer,
    .flags = buffer_handle_flags,
    .ready = buffer_handle_ready,
    .failed = buffer_handle_failed,
    .damage = buffer_handle_damage,
    .linux_dmabuf = buffer_handle_linux_dmabuf,
    .buffer_done = buffer_handle_buffer_done,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (!strcmp(interface, wl_shm_interface.name))
        s_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, wl_output_interface.name) && !s_output)
        s_output = wl_registry_bind(registry, name, &wl_output_interface, version < 3 ? version : 3);
    else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name))
        s_manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
}
static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }
static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sde-screenshot <out.ppm>\n");
        return 2;
    }
    s_out_path = argv[1];

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "sde-screenshot: cannot connect to a Wayland compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!s_shm || !s_output || !s_manager) {
        fprintf(stderr, "sde-screenshot: missing globals (shm=%p output=%p manager=%p)\n", (void*)s_shm, (void*)s_output, (void*)s_manager);
        return 1;
    }
    struct zwlr_screencopy_frame_v1* frame = zwlr_screencopy_manager_v1_capture_output(s_manager, 0, s_output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);

    while (!s_done && wl_display_dispatch(display) != -1) { }
    wl_display_disconnect(display);
    return s_done ? 0 : 1;
}
