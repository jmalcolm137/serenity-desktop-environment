/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Structured fuzzer for the data-device/selection/drag setup path: sources
 * with random MIME offers and actions, selections with random serials, drags
 * with and without icons, and source destruction at every stage. The
 * compositor must keep running; a protocol error for a bad serial is fine.
 *
 * Usage: data-device-fuzz-test [--cases N]
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

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct wl_data_device_manager* data_device_manager;
static struct wl_data_device* data_device;
static struct wl_surface* origin;
static struct wl_surface* icon;
static struct wl_buffer* icon_buffer;
static uint64_t rng_state = 1;
static unsigned drags_started;
static unsigned protocol_error;

static uint32_t next_u32(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

static struct wl_buffer* create_buffer(void)
{
    int const width = 32;
    int const height = 32;
    int const stride = width * 4;
    int const size = stride * height;
    int fd = memfd_create("data-device-fuzz", 0);
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
        ((uint32_t*)data)[i] = 0xFF406080;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* result = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return result;
}

// --- data source ---------------------------------------------------------

static void source_target(void* data, struct wl_data_source* source, const char* mime_type)
{
    (void)data;
    (void)source;
    (void)mime_type;
}

static void source_send(void* data, struct wl_data_source* source, const char* mime_type, int32_t fd)
{
    (void)data;
    (void)source;
    (void)mime_type;
    close(fd);
}

static void source_cancelled(void* data, struct wl_data_source* source)
{
    (void)data;
    (void)source;
}

static void source_drop_performed(void* data, struct wl_data_source* source)
{
    (void)data;
    (void)source;
}

static void source_finished(void* data, struct wl_data_source* source)
{
    (void)data;
    (void)source;
}

static void source_action(void* data, struct wl_data_source* source, uint32_t action)
{
    (void)data;
    (void)source;
    (void)action;
}

static struct wl_data_source_listener const source_listener = {
    source_target,
    source_send,
    source_cancelled,
    source_drop_performed,
    source_finished,
    source_action,
};

// --- data offers ---------------------------------------------------------
// wl_data_offer has three events: offer, source_actions and action. The
// accept/receive/set_actions/finish requests are not used here; the fuzzer only
// needs to keep the offer alive while the compositor negotiates it.

static void offer_offer(void* data, struct wl_data_offer* offer, const char* mime_type)
{
    (void)data;
    (void)offer;
    (void)mime_type;
}

static void offer_source_actions(void* data, struct wl_data_offer* offer, uint32_t actions)
{
    (void)data;
    (void)offer;
    (void)actions;
}

static void offer_action(void* data, struct wl_data_offer* offer, uint32_t action)
{
    (void)data;
    (void)offer;
    (void)action;
}

static struct wl_data_offer_listener const offer_listener = {
    offer_offer,
    offer_source_actions,
    offer_action,
};

static void device_data_offer(void* data, struct wl_data_device* device, struct wl_data_offer* offer)
{
    (void)data;
    (void)device;
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
}

static void device_enter(void* data, struct wl_data_device* device, uint32_t serial, struct wl_surface* surface, int32_t x, int32_t y, struct wl_data_offer* offer)
{
    (void)data;
    (void)device;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    (void)offer;
}

static void device_leave(void* data, struct wl_data_device* device)
{
    (void)data;
    (void)device;
}

static void device_motion(void* data, struct wl_data_device* device, uint32_t time, int32_t x, int32_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static void device_drop(void* data, struct wl_data_device* device)
{
    (void)data;
    (void)device;
}

static void device_selection(void* data, struct wl_data_device* device, struct wl_data_offer* offer)
{
    (void)data;
    (void)device;
    (void)offer;
}

static struct wl_data_device_listener const device_listener = {
    device_data_offer,
    device_enter,
    device_leave,
    device_motion,
    device_drop,
    device_selection,
};

// --- registry ------------------------------------------------------------

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_remove };

static void make_mime(char* buffer, size_t size)
{
    static char const* const known[] = {
        "text/plain",
        "text/plain;charset=utf-8",
        "text/html",
        "text/uri-list",
        "image/png",
        "application/x-serenity-clipboard",
    };
    if (next_u32() % 2 == 0) {
        snprintf(buffer, size, "%s", known[next_u32() % (sizeof(known) / sizeof(known[0]))]);
        return;
    }
    size_t length = 1 + next_u32() % (size - 1);
    for (size_t i = 0; i < length; ++i) {
        // Printable ASCII, but with an occasional slash/underscore; no NUL.
        buffer[i] = (char)('!' + next_u32() % 94);
    }
    buffer[length] = '\0';
}

static struct wl_data_source* create_source(unsigned* offers)
{
    struct wl_data_source* source = wl_data_device_manager_create_data_source(data_device_manager);
    wl_data_source_add_listener(source, &source_listener, NULL);
    unsigned const count = next_u32() % 5;
    char mime[64];
    for (unsigned i = 0; i < count; ++i) {
        make_mime(mime, sizeof(mime));
        wl_data_source_offer(source, mime);
    }
    static uint32_t const actions[] = {
        WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK,
    };
    wl_data_source_set_actions(source, actions[next_u32() % (sizeof(actions) / sizeof(actions[0]))]);
    *offers = count;
    return source;
}

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
        fprintf(stderr, "data-device-fuzz-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !seat || !data_device_manager) {
        fprintf(stderr, "data-device-fuzz-test: missing compositor/shm/seat/data-device-manager\n");
        return 1;
    }

    data_device = wl_data_device_manager_get_data_device(data_device_manager, seat);
    wl_data_device_add_listener(data_device, &device_listener, NULL);
    origin = wl_compositor_create_surface(compositor);
    icon = wl_compositor_create_surface(compositor);
    icon_buffer = create_buffer();
    wl_surface_attach(icon, icon_buffer, 0, 0);
    wl_surface_damage_buffer(icon, 0, 0, 32, 32);
    wl_surface_commit(icon);
    wl_display_roundtrip(display);

    for (unsigned i = 0; i < cases && !protocol_error; ++i) {
        unsigned offers = 0;
        struct wl_data_source* source = create_source(&offers);
        uint32_t serial = next_u32() % 4 == 0 ? 0 : next_u32();
        switch (next_u32() % 6) {
        case 0:
            // A plain selection; an unknown serial must be ignored or rejected,
            // never freed twice.
            wl_data_device_set_selection(data_device, source, serial);
            break;
        case 1:
            // Transfer the source to a drag and destroy it immediately: the
            // cancellation path must not leave a dangling drag source.
            wl_data_device_start_drag(data_device, source, origin, next_u32() % 2 ? icon : NULL, serial);
            ++drags_started;
            break;
        case 2:
            // Drag with an icon and let the source live until the compositor
            // cancels it (invalid serial cleanup).
            wl_data_device_start_drag(data_device, source, origin, icon, serial);
            ++drags_started;
            break;
        case 3:
            // No drag at all: a source with offers that is destroyed unused.
            wl_data_source_destroy(source);
            break;
        case 4:
            // Selection and then replace it; the previous source is destroyed by
            // the client, as a real clipboard owner does.
            wl_data_device_set_selection(data_device, source, 0);
            break;
        default:
            // Start a drag without an icon and then clear the selection; mixes
            // the two ownership paths.
            wl_data_device_start_drag(data_device, source, origin, NULL, serial);
            ++drags_started;
            wl_data_device_set_selection(data_device, NULL, 0);
            break;
        }
        if (wl_display_flush(display) < 0 || wl_display_get_error(display)) {
            protocol_error = 1;
            break;
        }
        if (wl_display_dispatch_pending(display) < 0 || wl_display_get_error(display)) {
            protocol_error = 1;
            break;
        }
    }

    fprintf(stderr, "DATA-DEVICE-FUZZ: %u cases, %u drags, protocol_error=%u\n", cases, drags_started, protocol_error);
    return 0;
}
