/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ext-workspace-test — a scripted ext-workspace-v1 client (R4.8). It binds the
// workspace manager, prints SDE's workspaces (id / name / coordinates / state)
// and optionally activates one, so the standard workspace protocol can be
// exercised without waybar.
//
//   ext-workspace-test        # list the workspaces
//   ext-workspace-test 3      # also activate the 3rd workspace

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "ext-workspace-v1-client-protocol.h"

#define MAX_WORKSPACES 64

struct workspace {
    struct ext_workspace_handle_v1* handle;
    char* id;
    char* name;
    uint32_t coords[4];
    size_t coords_len;
    uint32_t state;
};

static struct ext_workspace_manager_v1* manager = NULL;
static struct workspace workspaces[MAX_WORKSPACES];
static size_t workspace_count = 0;
static int activate_index = -1; // 0-based; -1 = don't activate
static int activated = 0;

static void handle_id(void* data, struct ext_workspace_handle_v1* handle, const char* id)
{
    (void)handle;
    struct workspace* workspace = data;
    free(workspace->id);
    workspace->id = strdup(id);
}

static void handle_name(void* data, struct ext_workspace_handle_v1* handle, const char* name)
{
    (void)handle;
    struct workspace* workspace = data;
    free(workspace->name);
    workspace->name = strdup(name);
}

static void handle_coordinates(void* data, struct ext_workspace_handle_v1* handle, struct wl_array* coordinates)
{
    (void)handle;
    struct workspace* workspace = data;
    workspace->coords_len = 0;
    if (coordinates->size % sizeof(uint32_t) != 0)
        return;
    size_t count = coordinates->size / sizeof(uint32_t);
    if (count > 4)
        count = 4;
    memcpy(workspace->coords, coordinates->data, count * sizeof(uint32_t));
    workspace->coords_len = count;
}

static void handle_state(void* data, struct ext_workspace_handle_v1* handle, uint32_t state)
{
    (void)handle;
    ((struct workspace*)data)->state = state;
}

static void handle_capabilities(void* data, struct ext_workspace_handle_v1* handle, uint32_t capabilities)
{
    (void)data;
    (void)handle;
    (void)capabilities;
}

static void handle_removed(void* data, struct ext_workspace_handle_v1* handle)
{
    (void)data;
    ext_workspace_handle_v1_destroy(handle);
}

static const struct ext_workspace_handle_v1_listener handle_listener = {
    .id = handle_id,
    .name = handle_name,
    .coordinates = handle_coordinates,
    .state = handle_state,
    .capabilities = handle_capabilities,
    .removed = handle_removed,
};

static void manager_workspace_group(void* data, struct ext_workspace_manager_v1* manager_, struct ext_workspace_group_handle_v1* group)
{
    (void)data;
    (void)manager_;
    (void)group;
}

static void manager_workspace(void* data, struct ext_workspace_manager_v1* manager_, struct ext_workspace_handle_v1* handle)
{
    (void)data;
    (void)manager_;
    if (workspace_count >= MAX_WORKSPACES) {
        ext_workspace_handle_v1_destroy(handle);
        return;
    }
    struct workspace* workspace = &workspaces[workspace_count++];
    workspace->handle = handle;
    ext_workspace_handle_v1_add_listener(handle, &handle_listener, workspace);
}

static void manager_done(void* data, struct ext_workspace_manager_v1* manager_)
{
    (void)data;
    printf("ext-workspace-test: %zu workspaces\n", workspace_count);
    for (size_t i = 0; i < workspace_count; ++i) {
        struct workspace* workspace = &workspaces[i];
        printf("ext-workspace-test: workspace %zu id=%s name=%s active=%d coords=", i,
            workspace->id ? workspace->id : "?", workspace->name ? workspace->name : "?",
            (workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) ? 1 : 0);
        for (size_t j = 0; j < workspace->coords_len; ++j)
            printf("%s%u", j ? "," : "", workspace->coords[j]);
        printf("\n");
    }
    if (activate_index >= 0 && (size_t)activate_index < workspace_count) {
        printf("ext-workspace-test: activating workspace %d\n", activate_index + 1);
        ext_workspace_handle_v1_activate(workspaces[activate_index].handle);
        ext_workspace_manager_v1_commit(manager_);
        activated = 1;
    }
}

static void manager_finished(void* data, struct ext_workspace_manager_v1* manager_)
{
    (void)data;
    (void)manager_;
}

static const struct ext_workspace_manager_v1_listener manager_listener = {
    .workspace_group = manager_workspace_group,
    .workspace = manager_workspace,
    .done = manager_done,
    .finished = manager_finished,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, 1);
        ext_workspace_manager_v1_add_listener(manager, &manager_listener, NULL);
    }
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char** argv)
{
    if (argc > 1)
        activate_index = atoi(argv[1]) - 1; // 1-based on the command line

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "ext-workspace-test: cannot connect to a Wayland display\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display); // globals

    if (!manager) {
        fprintf(stderr, "ext-workspace-test: compositor does not expose ext_workspace_manager_v1\n");
        wl_display_disconnect(display);
        return 1;
    }

    wl_display_roundtrip(display); // workspace handles
    wl_display_roundtrip(display); // their state, then done (which may activate)
    if (activated)
        wl_display_roundtrip(display);

    wl_display_disconnect(display);
    return workspace_count > 0 ? 0 : 2;
}
