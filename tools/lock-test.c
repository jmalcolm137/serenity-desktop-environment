/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Minimal ext-session-lock-v1 client for the R4.1 test
// (docs/11-r4-implementation-plan.md): lock the session, cover every output with
// a black lock surface, wait for the compositor's "locked" event, then unlock.
//
//   --hold MS     how long the session stays locked before unlocking (default
//                 800).
//   --no-unlock   lock and never unlock (the driver kills us).
//
// Prints "LOCK-TEST: locked" when the compositor confirms the lock.

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct ext_session_lock_manager_v1* lock_manager;
static struct ext_session_lock_v1* lock;
static struct wl_list outputs; // Output::link

struct Output {
    struct wl_output* output;
    struct wl_list link;
};

static int g_hold_ms = 800;
static bool g_no_unlock = false;
static long g_locked_at = -1;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct wl_buffer* create_black_buffer(int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    char name[] = "/tmp/lock-test-XXXXXX";
    int fd = mkstemp(name);
    unlink(name);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memset(data, 0, size);
    munmap(data, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void lock_surface_configure(void* data, struct ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height)
{
    struct wl_surface* wl_surface = data;
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    struct wl_buffer* buffer = create_black_buffer((int)width, (int)height);
    if (buffer) {
        wl_surface_attach(wl_surface, buffer, 0, 0);
        wl_surface_damage_buffer(wl_surface, 0, 0, (int)width, (int)height);
        wl_surface_commit(wl_surface);
        wl_buffer_destroy(buffer);
    }
    printf("LOCK-TEST: surface configured %ux%u\n", width, height);
    fflush(stdout);
}

static struct ext_session_lock_surface_v1_listener const lock_surface_listener = {
    .configure = lock_surface_configure,
};

static void create_lock_surfaces(void)
{
    struct Output* out;
    wl_list_for_each(out, &outputs, link)
    {
        struct wl_surface* surface = wl_compositor_create_surface(compositor);
        struct ext_session_lock_surface_v1* lock_surface = ext_session_lock_v1_get_lock_surface(lock, surface, out->output);
        ext_session_lock_surface_v1_add_listener(lock_surface, &lock_surface_listener, surface);
        printf("LOCK-TEST: lock surface created\n");
    }
    fflush(stdout);
}

static void lock_locked(void* data, struct ext_session_lock_v1*)
{
    (void)data;
    g_locked_at = now_ms();
    printf("LOCK-TEST: locked\n");
    fflush(stdout);
}

static void lock_finished(void* data, struct ext_session_lock_v1*)
{
    (void)data;
    printf("LOCK-TEST: finished\n");
    fflush(stdout);
}

static struct ext_session_lock_v1_listener const lock_listener = {
    .locked = lock_locked,
    .finished = lock_finished,
};

static void registry_global(void*, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0)
        lock_manager = wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct Output* out = calloc(1, sizeof(struct Output));
        out->output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        wl_list_insert(&outputs, &out->link);
    }
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }

static struct wl_registry_listener const registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char** argv)
{
    wl_list_init(&outputs);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc)
            g_hold_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-unlock") == 0)
            g_no_unlock = true;
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "lock-test: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // outputs

    if (!compositor || !shm || !lock_manager) {
        fprintf(stderr, "lock-test: missing compositor/shm/session-lock\n");
        return 1;
    }

    lock = ext_session_lock_manager_v1_lock(lock_manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    create_lock_surfaces();
    wl_display_flush(display);

    bool unlocked = false;
    for (int elapsed = 0; !g_no_unlock && elapsed < g_hold_ms + 2000; elapsed += 20) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);

        if (!unlocked && g_locked_at >= 0 && now_ms() - g_locked_at >= g_hold_ms) {
            printf("LOCK-TEST: unlocking\n");
            fflush(stdout);
            ext_session_lock_v1_unlock_and_destroy(lock);
            unlocked = true;
        }
    }

    if (g_no_unlock) {
        // Keep the lock alive; the driver kills us.
        while (true) {
            wl_display_dispatch(display);
        }
    }

    wl_display_disconnect(display);
    return 0;
}
