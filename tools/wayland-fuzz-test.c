/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A structured fuzzer for the xdg-shell surface/toplevel path: create a real
 * window, then send valid but extreme requests (titles, min/max sizes, states,
 * window geometry, damage, scale, transforms, move/resize/show-menu) in a
 * deterministic random order. The compositor must clamp or reject them and keep
 * serving; a protocol error that disconnects this client is acceptable, a crash
 * is not.
 *
 * Usage: wayland-fuzz-test [--cases N]
 * Env:   SDE_FUZZ_SEED (default 1), SDE_FUZZ_CASES (default 500)
 *
 * Not part of the desktop: a test tool.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_seat* seat;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;
static struct wl_buffer* buffer;
static int width = 960;
static int height = 600;
static int configured;
static int closed;

static uint64_t rng_state = 1;

static uint32_t next_u32(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

static int next_i32(void) { return (int32_t)next_u32(); }

static int pick_i32(void)
{
    static int const values[] = { 0, 1, 2, 4, 16, 100, 200, 320, 1024, 65535, -1, -100, -65535, 2147483647, -2147483647 - 1 };
    if (next_u32() % 3 == 0)
        return next_i32();
    return values[next_u32() % (sizeof(values) / sizeof(values[0]))];
}

static int pick_dimension(void)
{
    static int const values[] = { 0, 1, 2, 4, 16, 100, 200, 320, 1024, 4096, 16384, 32767, 65535 };
    if (next_u32() % 3 == 0)
        return (int)(next_u32() & 0x7FFF);
    return values[next_u32() % (sizeof(values) / sizeof(values[0]))];
}

static int pick_geometry_dimension(void)
{
    // xdg_surface.set_window_geometry requires positive width/height.
    return 1 + pick_dimension() % 0x7FFF;
}

static void random_string(char* out, size_t size)
{
    size_t length = next_u32() % size;
    if (next_u32() % 8 == 0)
        length = 0;
    for (size_t i = 0; i < length; ++i)
        out[i] = (char)('!' + next_u32() % 90);
    out[length] = '\0';
}

static uint32_t next_edges(void)
{
    // xdg-shell's resize_edge enum accepts the four individual edges (libwayland
    // validates enum values, not arbitrary bit combinations).
    static uint32_t const edges[] = { XDG_TOPLEVEL_RESIZE_EDGE_TOP, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
        XDG_TOPLEVEL_RESIZE_EDGE_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_RIGHT };
    return edges[next_u32() % (sizeof(edges) / sizeof(edges[0]))];
}

static struct wl_buffer* create_buffer(int w, int h)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("wayland-fuzz", 0);
    if (fd < 0) {
        perror("memfd_create");
        exit(1);
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        exit(1);
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    for (int i = 0; i < w * h; i++)
        ((uint32_t*)data)[i] = 0xFF4040C0;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* result = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return result;
}

static void commit_frame(void)
{
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

static void wm_base_ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static struct xdg_wm_base_listener const wm_base_listener = { wm_base_ping };

static void xdg_surface_configure(void* data, struct xdg_surface* surf, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surf, serial);
    commit_frame();
    configured = 1;
}

static struct xdg_surface_listener const xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void* data, struct xdg_toplevel* tl, int32_t w, int32_t h, struct wl_array* states)
{
    (void)data;
    (void)tl;
    (void)states;
    // The compositor's size is a suggestion; keep the shm buffer at 320x200 and
    // commit it again, which is what a client that ignores the size may do.
    (void)w;
    (void)h;
    if (configured)
        commit_frame();
}

static void toplevel_close(void* data, struct xdg_toplevel* tl)
{
    (void)data;
    (void)tl;
    closed = 1;
}

static struct xdg_toplevel_listener const toplevel_listener = { toplevel_configure, toplevel_close };

static void seat_capabilities(void* data, struct wl_seat* s, uint32_t capabilities)
{
    (void)data;
    (void)capabilities;
    (void)s;
}

static struct wl_seat_listener const seat_listener = { seat_capabilities };

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_remove };

int main(int argc, char** argv)
{
    unsigned cases = 500;
    if (getenv("SDE_FUZZ_CASES") && *getenv("SDE_FUZZ_CASES"))
        cases = (unsigned)strtoul(getenv("SDE_FUZZ_CASES"), NULL, 10);
    if (getenv("SDE_FUZZ_SEED") && *getenv("SDE_FUZZ_SEED"))
        rng_state = strtoull(getenv("SDE_FUZZ_SEED"), NULL, 10) | 1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cases") == 0 && i + 1 < argc)
            cases = (unsigned)strtoul(argv[++i], NULL, 10);
    }
    if (cases == 0)
        cases = 1;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "wayland-fuzz-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "wayland-fuzz-test: missing compositor/shm/xdg_wm_base\n");
        return 1;
    }

    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Wayland Fuzz Target");
    xdg_toplevel_set_app_id(toplevel, "sde-wayland-fuzz");
    buffer = create_buffer(width, height);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    uint32_t serial = 1;
    int verbose = getenv("SDE_FUZZ_VERBOSE") && *getenv("SDE_FUZZ_VERBOSE");
    for (unsigned i = 0; i < cases && !closed; ++i) {
        char text[300];
        unsigned op = next_u32() % 13;
        if (verbose)
            fprintf(stderr, "op %u type %u\n", i, op);
        switch (op) {
        case 0:
            random_string(text, sizeof(text));
            xdg_toplevel_set_title(toplevel, text);
            break;
        case 1:
            random_string(text, sizeof(text));
            xdg_toplevel_set_app_id(toplevel, text);
            break;
        case 2:
            // Keep the pair valid at commit: 0 max means "no maximum".
            xdg_toplevel_set_min_size(toplevel, pick_dimension(), pick_dimension());
            xdg_toplevel_set_max_size(toplevel, 0, 0);
            break;
        case 3:
            xdg_toplevel_set_min_size(toplevel, 0, 0);
            xdg_toplevel_set_max_size(toplevel, 1 + pick_dimension() % 0x7FFF, 1 + pick_dimension() % 0x7FFF);
            break;
        case 4:
            if (next_u32() & 1)
                xdg_toplevel_set_maximized(toplevel);
            else
                xdg_toplevel_unset_maximized(toplevel);
            break;
        case 5:
            if (next_u32() & 1)
                xdg_toplevel_set_minimized(toplevel);
            break;
        case 6:
            if (next_u32() & 1)
                xdg_toplevel_set_fullscreen(toplevel, NULL);
            else
                xdg_toplevel_unset_fullscreen(toplevel);
            break;
        case 7:
            xdg_surface_set_window_geometry(xdg_surface, next_i32(), next_i32(), pick_geometry_dimension(), pick_geometry_dimension());
            break;
        case 8: {
            int w = pick_i32() & 0x7FFF;
            int h = pick_i32() & 0x7FFF;
            wl_surface_damage_buffer(surface, 0, 0, w, h);
            break;
        }
        case 9:
            wl_surface_set_buffer_scale(surface, 1 + next_u32() % 4);
            break;
        case 10:
            wl_surface_set_buffer_transform(surface, next_u32() % 8);
            break;
        case 11:
            if (seat)
                xdg_toplevel_show_window_menu(toplevel, seat, serial++, pick_i32(), pick_i32());
            break;
        default:
            if (seat) {
                if (next_u32() & 1)
                    xdg_toplevel_move(toplevel, seat, serial++);
                else
                    xdg_toplevel_resize(toplevel, seat, serial++, next_edges());
            }
            break;
        }
        if ((i % 3) == 0) {
            commit_frame();
            wl_display_flush(display);
        }
        if (wl_display_dispatch_pending(display) < 0 || wl_display_get_error(display))
            break;
        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0 && wl_display_dispatch(display) < 0)
            break;
    }

    commit_frame();
    wl_display_flush(display);
    usleep(200 * 1000);
    fprintf(stderr, "WAYLAND-FUZZ: sent %u operations (configured=%d closed=%d)\n", cases, configured, closed);
    return 0;
}
