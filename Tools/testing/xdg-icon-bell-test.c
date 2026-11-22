// Test client: sets an xdg-toplevel-icon-v1 icon on a toplevel and rings the
// xdg-system-bell-v1, so the headless harness can assert the compositor handles
// both (docs/21-r5-protocol-parity-plan.md).
//
// SPDX-License-Identifier: BSD-2-Clause
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"

static struct wl_compositor* s_compositor;
static struct wl_shm* s_shm;
static struct xdg_wm_base* s_wm_base;
static struct xdg_toplevel_icon_manager_v1* s_icon_manager;
static struct xdg_system_bell_v1* s_system_bell;

static void wm_base_ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static struct xdg_wm_base_listener const s_wm_base_listener = { wm_base_ping };

static void toplevel_configure(void*, struct xdg_toplevel*, int32_t, int32_t, struct wl_array*) { }
static void toplevel_close(void*, struct xdg_toplevel*) { }
static struct xdg_toplevel_listener const s_toplevel_listener = { toplevel_configure, toplevel_close };

static void registry_global(void*, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        s_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        s_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        s_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0)
        s_icon_manager = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    else if (strcmp(interface, xdg_system_bell_v1_interface.name) == 0)
        s_system_bell = wl_registry_bind(registry, name, &xdg_system_bell_v1_interface, 1);
}
static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }
static struct wl_registry_listener const s_registry_listener = { registry_global, registry_global_remove };

static struct wl_buffer* create_buffer(int width, int height, uint32_t argb)
{
    int const stride = width * 4;
    int const size = stride * height;
    char name[] = "/tmp/xdg-icon-bell-XXXXXX";
    int fd = mkstemp(name);
    unlink(name);
    if (fd < 0 || ftruncate(fd, size) < 0)
        return NULL;
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    uint32_t* pixels = data;
    for (int i = 0; i < width * height; ++i)
        pixels[i] = argb;
    struct wl_shm_pool* pool = wl_shm_create_pool(s_shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

int main(void)
{
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "xdg-icon-bell-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!s_compositor || !s_shm || !s_wm_base || !s_icon_manager || !s_system_bell) {
        fprintf(stderr, "xdg-icon-bell-test: missing a required global\n");
        return 2;
    }

    xdg_wm_base_add_listener(s_wm_base, &s_wm_base_listener, NULL);
    struct wl_surface* surface = wl_compositor_create_surface(s_compositor);
    struct xdg_surface* xdg_surface = xdg_wm_base_get_xdg_surface(s_wm_base, surface);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &s_toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "xdg-icon-bell-test");
    xdg_toplevel_set_app_id(toplevel, "xdg-icon-bell-test");

    // A 16x16 icon (opaque magenta), set as the toplevel's icon.
    struct xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(s_icon_manager);
    struct wl_buffer* buffer = create_buffer(16, 16, 0xffff00ffu);
    xdg_toplevel_icon_v1_add_buffer(icon, buffer, 1);
    xdg_toplevel_icon_v1_set_name(icon, "xdg-icon-bell-test");
    xdg_toplevel_icon_manager_v1_set_icon(s_icon_manager, toplevel, icon);

    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Ring the system bell (attached to the surface).
    xdg_system_bell_v1_ring(s_system_bell, surface);
    wl_display_flush(display);

    // Keep the toplevel mapped long enough for the compositor to apply the icon.
    usleep(800 * 1000);

    xdg_toplevel_icon_v1_destroy(icon);
    wl_buffer_destroy(buffer);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}
