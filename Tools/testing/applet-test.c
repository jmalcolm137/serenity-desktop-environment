/*
 * Minimal applet (wlr-layer-shell) client used to exercise the compositor's
 * applet-area layout (WS4). Not part of the desktop: a test tool.
 *
 * The compositor identifies applets by the "serenity-applet[:<name>]" layer
 * namespace, so this tool lets a test create several applets with chosen names
 * and sizes.
 *
 * SPDX-License-Identifier: BSD-2-Clause
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
static int mapped_width;
static int mapped_height;

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
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

static struct wl_buffer* create_buffer(int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("applet-test", 0);
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
    for (int i = 0; i < width * height; i++)
        ((uint32_t*)data)[i] = 0xFF30A0E0;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static struct wl_buffer* current_buffer;
static int current_buffer_width;
static int current_buffer_height;

static void layer_configure(void* data, struct zwlr_layer_surface_v1* layer_surface, uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    int new_width = width > 0 ? (int)width : mapped_width;
    int new_height = height > 0 ? (int)height : mapped_height;
    bool const changed = !current_buffer || new_width != current_buffer_width || new_height != current_buffer_height;
    mapped_width = new_width;
    mapped_height = new_height;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    // Commit only when there is something new to attach, as the LibWindowServer
    // client does: committing on every configure would spin against a compositor
    // that reconfigures on every commit.
    if (!changed)
        return;
    if (current_buffer)
        wl_buffer_destroy(current_buffer);
    current_buffer = create_buffer(mapped_width, mapped_height);
    current_buffer_width = mapped_width;
    current_buffer_height = mapped_height;
    wl_surface_attach(surface, current_buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, mapped_width, mapped_height);
    wl_surface_commit(surface);
}

static void layer_closed(void* data, struct zwlr_layer_surface_v1* layer_surface)
{
    (void)data;
    (void)layer_surface;
    exit(0);
}

static struct zwlr_layer_surface_v1_listener const layer_listener = { layer_configure, layer_closed };

int main(int argc, char** argv)
{
    char const* namespace = "serenity-applet:Test";
    mapped_width = 40;
    mapped_height = 21;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--namespace") == 0 && i + 1 < argc)
            namespace = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            mapped_width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            mapped_height = atoi(argv[++i]);
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "applet-test: cannot connect to a compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "applet-test: missing compositor/shm/layer-shell globals\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    struct zwlr_layer_surface_v1* layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, namespace);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, mapped_width, mapped_height);
    zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    wl_surface_commit(surface);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    fprintf(stderr, "applet-test: '%s' mapped %dx%d\n", namespace, mapped_width, mapped_height);

    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
