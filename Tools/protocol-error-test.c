/*
 * R6/N4: a client that deliberately violates a wayland protocol rule, to check
 * that the compositor isolates the error and keeps serving other clients. The
 * error is delivered to *this* client (its connection is dropped); the
 * compositor must survive.
 *
 * Usage: protocol-error-test [scale0|shm-size]
 *   scale0    -- wl_surface.set_buffer_scale(0)  -> invalid_scale
 *   shm-size  -- wl_shm.create_pool(fd, -1)      -> invalid size
 *
 * Exit status 0 means the protocol error was observed (expected).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor* compositor;
static struct wl_shm* shm;

static void registry_global(void* data, struct wl_registry* registry, uint32_t name,
    char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(int argc, char** argv)
{
    char const* mode = argc > 1 ? argv[1] : "scale0";

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "protocol-error-test: cannot connect to the compositor\n");
        return 2;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (strcmp(mode, "shm-size") == 0) {
        if (!shm) {
            fprintf(stderr, "protocol-error-test: no wl_shm\n");
            return 2;
        }
        // The size check fires before the fd is used.
        int fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
        wl_shm_create_pool(shm, fd, -1);
    } else {
        struct wl_surface* surface = wl_compositor_create_surface(compositor);
        wl_surface_set_buffer_scale(surface, 0);
    }

    // The compositor posts the error and drops the connection; a roundtrip then
    // fails (EPROTO/EPIPE). Allow a few for the message to be delivered.
    for (int i = 0; i < 5; i++) {
        if (wl_display_roundtrip(display) < 0) {
            printf("protocol-error-test: %s: protocol error delivered (expected)\n", mode);
            wl_display_disconnect(display);
            return 0;
        }
    }

    printf("protocol-error-test: %s: no protocol error observed (unexpected)\n", mode);
    wl_display_disconnect(display);
    return 1;
}
