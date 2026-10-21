/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A structured fuzzer for the layer-shell path: one layer surface driven with
 * valid-but-extreme anchors, exclusive zones, margins, sizes, layers and keyboard
 * interactivity. The compositor must arrange/reject them without crashing.
 *
 * Usage: layer-fuzz-test [--cases N]
 * Env:   SDE_FUZZ_SEED (default 1), SDE_FUZZ_CASES (default 500)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct zwlr_layer_shell_v1* layer_shell;
static struct wl_surface* surface;
static struct zwlr_layer_surface_v1* layer_surface;
static struct wl_buffer* buffer;
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

static int next_dimension(void)
{
    static int const values[] = { 0, 1, 2, 16, 64, 256, 512, 1024, 4096, 65535 };
    if (next_u32() % 3 == 0)
        return (int)(next_u32() & 0xFFFF);
    return values[next_u32() % (sizeof(values) / sizeof(values[0]))];
}

static struct wl_buffer* create_buffer(int w, int h)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("layer-fuzz", 0);
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
        ((uint32_t*)data)[i] = 0xFF304050;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* result = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return result;
}

static void render(void)
{
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, 960, 600);
    wl_surface_commit(surface);
}

static void layer_configure(void* data, struct zwlr_layer_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h)
{
    (void)data;
    (void)w;
    (void)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    render();
    configured = 1;
}

static void layer_closed(void* data, struct zwlr_layer_surface_v1* ls)
{
    (void)data;
    (void)ls;
    closed = 1;
}

static struct zwlr_layer_surface_v1_listener const layer_listener = { layer_configure, layer_closed };

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
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
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--cases") == 0)
            cases = (unsigned)strtoul(argv[i + 1], NULL, 10);
    }
    if (cases == 0)
        cases = 1;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "layer-fuzz-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "layer-fuzz-test: missing compositor/shm/layer-shell\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "sde-layer-fuzz");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    buffer = create_buffer(960, 600);
    zwlr_layer_surface_v1_set_size(layer_surface, 320, 200);
    zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    static uint32_t const anchors[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static uint32_t const layers[] = { ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY };
    static uint32_t const interactivity[] = { ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND };

    for (unsigned i = 0; i < cases && !closed; ++i) {
        switch (next_u32() % 7) {
        case 0:
            zwlr_layer_surface_v1_set_anchor(layer_surface, anchors[next_u32() % 16]);
            break;
        case 1:
            zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, (int32_t)next_u32());
            break;
        case 2:
            zwlr_layer_surface_v1_set_margin(layer_surface, (int32_t)next_u32(), (int32_t)next_u32(), (int32_t)next_u32(), (int32_t)next_u32());
            break;
        case 3:
            zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)next_dimension(), (uint32_t)next_dimension());
            break;
        case 4:
            zwlr_layer_surface_v1_set_layer(layer_surface, layers[next_u32() % 4]);
            break;
        case 5:
            zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, interactivity[next_u32() % 3]);
            break;
        default:
            wl_surface_set_buffer_scale(surface, 1 + next_u32() % 4);
            break;
        }
        render();
        if (wl_display_dispatch_pending(display) < 0 || wl_display_get_error(display))
            break;
    }

    fprintf(stderr, "LAYER-FUZZ: sent %u operations (configured=%d closed=%d)\n", cases, configured, closed);
    return 0;
}
