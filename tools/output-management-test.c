// output-management-test — disable or re-enable one output through
// zwlr_output_management_v1.
//
//   output-management-test --off <output-name>
//   output-management-test --on  <output-name> [<width>x<height>]
//
// A disabled head advertises no modes at all, so re-enabling one needs a size from
// the caller (`set_custom_mode`, the same way the compositor brings up a virtual
// output). Without it the request cannot be built and the compositor would refuse
// it.
//
// This is the compositor's hotplug path as a client can drive it: a *disabled*
// output still exists, but nothing of it is on screen any more, so its windows
// have to move to a surviving output and the primary may have to be reassigned.
// Unplugging a monitor is the same state arriving from the backend.
//
// It mirrors every other head's current mode/position/scale, because the protocol
// requires every head to appear in a configuration ("It is a protocol error to
// omit a head in a configuration") — the reason disabling needs `disable_head`
// rather than leaving the head out.
//
// Exit status: 0 applied, 1 the compositor refused, 2 usage or connection error.
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-management-unstable-v1-client-protocol.h"

#define MAX_MODES 32

struct mode {
    struct zwlr_output_mode_v1* mode;
    int width, height, refresh;
    bool preferred;
};

struct head {
    struct zwlr_output_head_v1* head;
    char* name;
    struct mode modes[MAX_MODES];
    size_t mode_count;
    int current_width, current_height, current_refresh;
    bool has_current_mode;
    int x, y, scale;
    bool enabled;
    struct head* next;
};

static struct zwlr_output_manager_v1* manager;
static struct head* heads;
static uint32_t serial;
static bool got_configuration;
static int result = 1;

static void mode_size(void* data, struct zwlr_output_mode_v1*, int32_t width, int32_t height)
{
    struct mode* mode = data;
    mode->width = width;
    mode->height = height;
}

static void mode_refresh(void* data, struct zwlr_output_mode_v1*, int32_t refresh)
{
    ((struct mode*)data)->refresh = refresh;
}

static void mode_preferred(void* data, struct zwlr_output_mode_v1*)
{
    ((struct mode*)data)->preferred = true;
}

static void mode_finished(void* data, struct zwlr_output_mode_v1* mode)
{
    ((struct mode*)data)->mode = NULL;
    zwlr_output_mode_v1_destroy(mode);
}

static struct zwlr_output_mode_v1_listener const mode_listener = {
    .size = mode_size,
    .refresh = mode_refresh,
    .preferred = mode_preferred,
    .finished = mode_finished,
};

static void head_name(void* data, struct zwlr_output_head_v1*, const char* name)
{
    struct head* head = data;
    free(head->name);
    head->name = strdup(name);
}

static void head_description(void*, struct zwlr_output_head_v1*, const char*) { }
static void head_physical_size(void*, struct zwlr_output_head_v1*, int32_t, int32_t) { }
static void head_make(void*, struct zwlr_output_head_v1*, const char*) { }
static void head_model(void*, struct zwlr_output_head_v1*, const char*) { }
static void head_serial_number(void*, struct zwlr_output_head_v1*, const char*) { }

static void head_mode(void* data, struct zwlr_output_head_v1*, struct zwlr_output_mode_v1* mode)
{
    struct head* head = data;
    if (head->mode_count >= MAX_MODES) {
        zwlr_output_mode_v1_destroy(mode);
        return;
    }
    struct mode* slot = &head->modes[head->mode_count++];
    memset(slot, 0, sizeof(*slot));
    slot->mode = mode;
    zwlr_output_mode_v1_add_listener(mode, &mode_listener, slot);
}

static void head_enabled(void* data, struct zwlr_output_head_v1*, int32_t enabled)
{
    ((struct head*)data)->enabled = enabled != 0;
}

static void head_current_mode(void* data, struct zwlr_output_head_v1*, struct zwlr_output_mode_v1* mode)
{
    struct head* head = data;
    for (size_t i = 0; i < head->mode_count; ++i) {
        if (head->modes[i].mode == mode) {
            head->current_width = head->modes[i].width;
            head->current_height = head->modes[i].height;
            head->current_refresh = head->modes[i].refresh;
            head->has_current_mode = true;
        }
    }
}

static void head_position(void* data, struct zwlr_output_head_v1*, int32_t x, int32_t y)
{
    struct head* head = data;
    head->x = x;
    head->y = y;
}

static void head_transform(void*, struct zwlr_output_head_v1*, int32_t) { }

static void head_scale(void* data, struct zwlr_output_head_v1*, wl_fixed_t scale)
{
    struct head* head = data;
    head->scale = wl_fixed_to_double(scale);
}

static void head_finished(void* data, struct zwlr_output_head_v1* head)
{
    struct head* entry = data;
    entry->head = NULL;
    zwlr_output_head_v1_destroy(head);
}

static void head_adaptive_sync(void*, struct zwlr_output_head_v1*, uint32_t) { }

static struct zwlr_output_head_v1_listener const head_listener = {
    .name = head_name,
    .description = head_description,
    .physical_size = head_physical_size,
    .mode = head_mode,
    .enabled = head_enabled,
    .current_mode = head_current_mode,
    .position = head_position,
    .transform = head_transform,
    .scale = head_scale,
    .finished = head_finished,
    .make = head_make,
    .model = head_model,
    .serial_number = head_serial_number,
    .adaptive_sync = head_adaptive_sync,
};

static void manager_head(void*, struct zwlr_output_manager_v1*, struct zwlr_output_head_v1* wl_head)
{
    static struct head* tail;
    struct head* entry = calloc(1, sizeof(*entry));
    entry->head = wl_head;
    entry->scale = 1;
    if (tail)
        tail->next = entry;
    else
        heads = entry;
    tail = entry;
    zwlr_output_head_v1_add_listener(wl_head, &head_listener, entry);
}

static void configuration_succeeded(void*, struct zwlr_output_configuration_v1* config)
{
    if (config)
        zwlr_output_configuration_v1_destroy(config);
    got_configuration = true;
    result = 0;
}

static void configuration_failed(void*, struct zwlr_output_configuration_v1* config)
{
    if (config)
        zwlr_output_configuration_v1_destroy(config);
    got_configuration = true;
    result = 1;
}

static void configuration_cancelled(void*, struct zwlr_output_configuration_v1* config)
{
    if (config)
        zwlr_output_configuration_v1_destroy(config);
    got_configuration = true;
    result = 1;
}

static struct zwlr_output_configuration_v1_listener const configuration_listener = {
    .succeeded = configuration_succeeded,
    .failed = configuration_failed,
    .cancelled = configuration_cancelled,
};

static const char* target_name;
static bool turn_off;
static int custom_width, custom_height;

static void manager_done(void*, struct zwlr_output_manager_v1*, uint32_t done_serial)
{
    serial = done_serial;
    struct head* target = NULL;
    for (struct head* entry = heads; entry; entry = entry->next) {
        if (entry->name && strcmp(entry->name, target_name) == 0)
            target = entry;
    }
    if (!target) {
        fprintf(stderr, "output-management-test: no output named '%s'\n", target_name);
        got_configuration = true;
        result = 2;
        return;
    }

    struct zwlr_output_configuration_v1* config = zwlr_output_manager_v1_create_configuration(manager, serial);
    zwlr_output_configuration_v1_add_listener(config, &configuration_listener, NULL);
    for (struct head* entry = heads; entry; entry = entry->next) {
        if (entry == target && turn_off) {
            // Heads omitted from a configuration are a protocol error, so a
            // disabled head has to be disabled explicitly.
            zwlr_output_configuration_v1_disable_head(config, entry->head);
            continue;
        }
        struct zwlr_output_configuration_head_v1* head = zwlr_output_configuration_v1_enable_head(config, entry->head);
        struct mode* chosen = NULL;
        for (size_t i = 0; i < entry->mode_count; ++i) {
            if (entry->modes[i].mode && entry->modes[i].preferred) {
                chosen = &entry->modes[i];
                break;
            }
        }
        if (!chosen && entry->mode_count > 0 && entry->modes[0].mode) {
            chosen = &entry->modes[0];
        }
        if (chosen) {
            zwlr_output_configuration_head_v1_set_mode(head, chosen->mode);
        } else if (entry == target && custom_width > 0 && custom_height > 0) {
            // A disabled head lists no modes; bring it up at the size the caller
            // gave, which is what the compositor does for a virtual output.
            zwlr_output_configuration_head_v1_set_custom_mode(head, custom_width, custom_height, 0);
        } else {
            fprintf(stderr, "output-management-test: '%s' advertises no mode; pass <width>x<height>\n",
                entry->name ? entry->name : "?");
            got_configuration = true;
            result = 2;
            zwlr_output_configuration_v1_destroy(config);
            return;
        }
        zwlr_output_configuration_head_v1_set_position(head, entry->x, entry->y);
        zwlr_output_configuration_head_v1_set_scale(head, wl_fixed_from_double(entry->scale > 0 ? entry->scale : 1));
    }
    zwlr_output_configuration_v1_apply(config);
}

static void manager_finished(void*, struct zwlr_output_manager_v1* manager)
{
    zwlr_output_manager_v1_destroy(manager);
}

static struct zwlr_output_manager_v1_listener const manager_listener = {
    .head = manager_head,
    .done = manager_done,
    .finished = manager_finished,
};

static void registry_global(void*, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, zwlr_output_manager_v1_interface.name) != 0)
        return;
    // disable_head is version 2; binding high keeps every head event available.
    manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, version > 4 ? 4 : version);
    zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }

static struct wl_registry_listener const registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char** argv)
{
    if (argc < 3 || (strcmp(argv[1], "--off") != 0 && strcmp(argv[1], "--on") != 0)) {
        fprintf(stderr, "usage: %s --off|--on <output-name> [<width>x<height>]\n", argv[0]);
        return 2;
    }
    turn_off = strcmp(argv[1], "--off") == 0;
    target_name = argv[2];
    if (argc >= 4)
        sscanf(argv[3], "%dx%d", &custom_width, &custom_height);

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "output-management-test: cannot connect to a compositor\n");
        return 2;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager) {
        fprintf(stderr, "output-management-test: the compositor does not offer zwlr_output_manager_v1\n");
        return 2;
    }
    // Two roundtrips: one for the heads, one for their modes and state.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    for (int i = 0; i < 200 && !got_configuration; ++i)
        wl_display_dispatch(display);

    wl_display_disconnect(display);
    return result;
}
