/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Scripted client for xdg-session-management-v1 (R4.2). Two modes:
//
//   session-test create            get a new session, map a toplevel named "win"
//   session-test restore <id>      restore the session and the "win" toplevel
//
// Prints markers the driver asserts on. Headless.

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-session-management-v1-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct xdg_session_manager_v1* session_manager;
static struct xdg_session_v1* session;
static char session_id[256];

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool g_configured = false;

static struct wl_buffer* make_buffer(int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    char name[] = "/tmp/session-test-XXXXXX";
    int fd = mkstemp(name);
    unlink(name);
    if (fd < 0 || ftruncate(fd, size) < 0)
        return NULL;
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
        return NULL;
    memset(data, 0x40, size);
    munmap(data, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void wm_base_ping(void* d, struct xdg_wm_base* b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void xdg_surface_configure(void* d, struct xdg_surface* s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    (void)d;
    g_configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };
static void toplevel_noop_cfg(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void toplevel_noop(void* d, struct xdg_toplevel* t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_noop_cfg,
    .close = toplevel_noop,
};

// --- session listeners ---
static const char* mode = "create";
static bool got_created = false;
static bool got_restored = false;

static void session_created(void* d, struct xdg_session_v1* s, const char* id)
{
    (void)d;
    (void)s;
    snprintf(session_id, sizeof(session_id), "%s", id);
    got_created = true;
    printf("SESSION-TEST: created id=%s\n", id);
    fflush(stdout);
}
static void session_restored_cb(void* d, struct xdg_session_v1* s) { (void)d; (void)s; got_restored = true; printf("SESSION-TEST: restored\n"); fflush(stdout); }
static void session_replaced(void* d, struct xdg_session_v1* s) { (void)d; (void)s; printf("SESSION-TEST: replaced\n"); fflush(stdout); }
static const struct xdg_session_v1_listener session_listener = {
    .created = session_created,
    .restored = session_restored_cb,
    .replaced = session_replaced,
};

static void ts_restored(void* d, struct xdg_toplevel_session_v1* ts) { (void)d; (void)ts; printf("SESSION-TEST: toplevel-restored\n"); fflush(stdout); }
static const struct xdg_toplevel_session_v1_listener ts_listener = { .restored = ts_restored };
static void ts_noop_cb(void* d, struct xdg_toplevel_session_v1* ts, const char* n) { (void)d; (void)ts; (void)n; }

static void registry_global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version)
{
    (void)d;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(iface, xdg_session_manager_v1_interface.name) == 0)
        session_manager = wl_registry_bind(r, name, &xdg_session_manager_v1_interface, 1);
}
static void registry_global_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

int main(int argc, char** argv)
{
    if (argc >= 2)
        mode = argv[1];
    const char* restore_id = (argc >= 3) ? argv[2] : NULL;

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "session-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base || !session_manager) {
        fprintf(stderr, "session-test: missing globals\n");
        return 1;
    }

    uint32_t reason = strcmp(mode, "restore") == 0 ? XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE : XDG_SESSION_MANAGER_V1_REASON_LAUNCH;
    session = xdg_session_manager_v1_get_session(session_manager, reason, restore_id);
    xdg_session_v1_add_listener(session, &session_listener, NULL);
    wl_display_roundtrip(display);

    // A toplevel named "win".
    struct wl_surface* surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Session Test");
    if (strcmp(mode, "restore") == 0) {
        struct xdg_toplevel_session_v1* ts = xdg_session_v1_restore_toplevel(session, toplevel, "win");
        xdg_toplevel_session_v1_add_listener(ts, &ts_listener, NULL);
    } else {
        struct xdg_toplevel_session_v1* ts = xdg_session_v1_add_toplevel(session, toplevel, "win");
        xdg_toplevel_session_v1_add_listener(ts, &ts_listener, NULL);
    }
    wl_surface_commit(surface);

    // Wait for the first xdg_surface configure before attaching a buffer (a
    // buffer committed before the configure is a protocol error).
    for (int i = 0; i < 20 && !g_configured; ++i)
        wl_display_roundtrip(display);
    printf("SESSION-TEST: configured=%d\n", g_configured ? 1 : 0);
    fflush(stdout);

    struct wl_buffer* buffer = make_buffer(220, 160);
    if (buffer) {
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, 220, 160);
        wl_surface_commit(surface);
    }

    long deadline = now_ms() + 1200;
    while (now_ms() < deadline) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
    }

    wl_display_disconnect(display);
    return 0;
}
