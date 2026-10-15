/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SessionManager.h"

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Model.h"

extern "C" {
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "xdg-session-management-v1-server-protocol.h"
}

namespace SessionManager {

struct ToplevelState {
    int x { 0 };
    int y { 0 };
    int width { 0 };
    int height { 0 };
    unsigned row { 0 };
    unsigned column { 0 };
};

struct Session;

struct ToplevelSession {
    wl_resource* resource { nullptr };
    Session* session { nullptr };
    Toplevel* toplevel { nullptr };
    ByteString name;
};

struct Session {
    Server* server { nullptr };
    wl_client* client { nullptr };
    wl_resource* resource { nullptr };
    ByteString id;
    Vector<NonnullOwnPtr<ToplevelSession>> toplevels;
    HashMap<ByteString, ToplevelState> states;
};

static Vector<NonnullOwnPtr<Session>> s_sessions;
static u64 s_id_counter { 0 };

static ByteString sessions_dir()
{
    if (auto const* dir = getenv("SDE_STATE_DIR"); dir && *dir)
        return ByteString::formatted("{}/sessions", dir);
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-sessions", runtime);
    return ByteString("/tmp/sde-sessions");
}

static ByteString state_path(ByteString const& id)
{
    return ByteString::formatted("{}/{}", sessions_dir(), id);
}

static void save_states(Session const& session)
{
    auto dir = sessions_dir();
    (void)::mkdir(dir.characters(), 0700); // best effort; EEXIST is fine

    // Write a temporary file and rename it over the state, so the file on disk is
    // always either the whole previous state or the whole new one. Truncating in place
    // left an empty file whenever the compositor stopped between the truncate and the
    // write -- and this runs on every geometry change, so a session could be lost to a
    // kill or a crash right after it recorded its windows.
    auto const path = state_path(session.id);
    auto const temporary = ByteString::formatted("{}.tmp", path);
    int fd = ::open(temporary.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    for (auto const& it : session.states) {
        auto line = ByteString::formatted("{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            it.key, it.value.x, it.value.y, it.value.width, it.value.height, it.value.row, it.value.column);
        (void)::write(fd, line.characters(), line.length());
    }
    ::close(fd);
    if (::rename(temporary.characters(), path.characters()) < 0)
        (void)::unlink(temporary.characters());
}

static void load_states(ByteString const& id, HashMap<ByteString, ToplevelState>& out)
{
    int fd = ::open(state_path(id).characters(), O_RDONLY);
    if (fd < 0)
        return;

    ByteString text;
    char chunk[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0)
        text = ByteString::formatted("{}{}", text, StringView { chunk, static_cast<size_t>(n) });
    ::close(fd);

    for (auto line : StringView { text }.split_view('\n')) {
        if (line.is_empty())
            continue;
        auto parts = line.split_view('\t');
        if (parts.size() != 7)
            continue;
        ToplevelState state;
        state.x = parts[1].to_number<int>().value_or(0);
        state.y = parts[2].to_number<int>().value_or(0);
        state.width = parts[3].to_number<int>().value_or(0);
        state.height = parts[4].to_number<int>().value_or(0);
        state.row = parts[5].to_number<unsigned>().value_or(0);
        state.column = parts[6].to_number<unsigned>().value_or(0);
        out.set(parts[0].to_byte_string(), state);
    }
}

static Toplevel* resolve_toplevel(Server& server, wl_resource* toplevel_resource)
{
    auto* xdg_toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (!xdg_toplevel)
        return nullptr;
    for (auto& toplevel : server.toplevels) {
        if (toplevel->xdg_toplevel == xdg_toplevel)
            return toplevel.ptr();
    }
    return nullptr;
}

static void toplevel_session_resource_destroy(wl_resource* resource)
{
    auto* entry = static_cast<ToplevelSession*>(wl_resource_get_user_data(resource));
    if (!entry)
        return;
    if (auto* session = entry->session) {
        session->toplevels.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == entry; });
    }
}

static void session_resource_destroy(wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;
    // The associated xdg_toplevel_session_v1 objects become inert.
    for (auto& entry : session->toplevels) {
        if (entry->resource)
            wl_resource_set_user_data(entry->resource, nullptr);
    }
    save_states(*session);
    s_sessions.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == session; });
}

static void session_add_toplevel_common(wl_client* client, wl_resource* resource, uint32_t new_id, wl_resource* toplevel_resource, char const* name, bool restore)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    ByteString entry_name = name ? ByteString { name } : ByteString {};

    if (entry_name.is_empty() || entry_name.contains('\t')) {
        wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_INVALID_NAME, "invalid toplevel name");
        return;
    }
    bool const known = session->states.contains(entry_name);
    if (!restore && known) {
        wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "toplevel name already in use");
        return;
    }

    auto* entry_resource = wl_resource_create(client, &xdg_toplevel_session_v1_interface, 1, new_id);
    if (!entry_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    auto entry = make<ToplevelSession>();
    entry->resource = entry_resource;
    entry->session = session;
    entry->name = move(entry_name);
    entry->toplevel = resolve_toplevel(*session->server, toplevel_resource);

    bool did_restore = false;
    if (entry->toplevel) {
        entry->toplevel->session_managed = true;
        entry->toplevel->session_id = session->id;
        entry->toplevel->session_name = entry->name;
        if (restore) {
            if (auto it = session->states.find(entry->name); it != session->states.end()) {
                auto const& state = it->value;
                entry->toplevel->session_restore_position = { state.x, state.y };
                entry->toplevel->has_session_restore_position = true;
                entry->toplevel->workspace_row = state.row;
                entry->toplevel->workspace_column = state.column;
                if (state.width > 0 && state.height > 0) {
                    // Defer to the initial commit: restore_toplevel runs before
                    // the toplevel is initialized, where set_size would assert.
                    entry->toplevel->session_restore_size = { state.width, state.height };
                    entry->toplevel->has_session_restore_size = true;
                }
                did_restore = true;
            }
        }
    }

    static const struct xdg_toplevel_session_v1_interface entry_impl = {
        .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
        .rename = [](wl_client*, wl_resource*, char const*) {},
    };
    wl_resource_set_implementation(entry_resource, &entry_impl, entry.ptr(), toplevel_session_resource_destroy);

    session->states.ensure(entry->name);
    session->toplevels.append(move(entry));

    if (did_restore)
        xdg_toplevel_session_v1_send_restored(entry_resource);
    save_states(*session);
}

static const struct xdg_session_v1_interface s_session_impl = {
    .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
    .remove = [](wl_client*, wl_resource* resource) {
        auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
        if (!session)
            return;
        session->states.clear();
        (void)::unlink(state_path(session->id).characters());
    },
    .add_toplevel = [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* toplevel, char const* name) {
        session_add_toplevel_common(client, resource, id, toplevel, name, false);
    },
    .restore_toplevel = [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* toplevel, char const* name) {
        session_add_toplevel_common(client, resource, id, toplevel, name, true);
    },
    .remove_toplevel = [](wl_client*, wl_resource* resource, char const* name) {
        auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
        if (!session || !name)
            return;
        ByteString entry_name { name };
        session->states.remove(entry_name);
        session->toplevels.remove_first_matching([&](auto const& entry) { return entry->name == entry_name; });
        save_states(*session);
    },
};

static void manager_get_session(wl_client* client, wl_resource* resource, uint32_t id, uint32_t reason, char const* session_id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    if (reason < 1 || reason > 3) {
        wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON, "invalid reason");
        return;
    }

    auto* session_resource = wl_resource_create(client, &xdg_session_v1_interface, 1, id);
    if (!session_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    auto session = make<Session>();
    session->server = &server;
    session->client = client;
    session->resource = session_resource;

    bool restored = false;
    if (session_id && *session_id) {
        session->id = session_id;
        load_states(session->id, session->states);
        restored = !session->states.is_empty();
        // If another client is using this session, it is replaced.
        for (auto& other : s_sessions) {
            if (other->id != session->id || other.ptr() == session.ptr())
                continue;
            if (other->resource)
                xdg_session_v1_send_replaced(other->resource);
            for (auto& entry : other->toplevels) {
                if (entry->resource)
                    wl_resource_set_user_data(entry->resource, nullptr);
            }
            other->resource = nullptr;
        }
    } else {
        session->id = ByteString::formatted("sde-{:x}-{:x}", static_cast<u64>(reinterpret_cast<uintptr_t>(client)), ++s_id_counter);
    }

    wl_resource_set_implementation(session_resource, &s_session_impl, session.ptr(), session_resource_destroy);
    bool const was_restored = restored;
    s_sessions.append(move(session));

    if (was_restored)
        xdg_session_v1_send_restored(session_resource);
    else
        xdg_session_v1_send_created(session_resource, s_sessions.last()->id.characters());
}

static const struct xdg_session_manager_v1_interface s_manager_impl = {
    .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
    .get_session = manager_get_session,
};

static void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* resource = wl_resource_create(client, &xdg_session_manager_v1_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &s_manager_impl, data, nullptr);
}

void initialize(Server& server)
{
    wl_global_create(server.display, &xdg_session_manager_v1_interface, 1, &server, manager_bind);
}

void note_geometry(Toplevel& toplevel)
{
    if (!toplevel.session_managed || toplevel.session_id.is_empty())
        return;

    for (auto& session : s_sessions) {
        if (session->id != toplevel.session_id)
            continue;
        ToplevelState state;
        state.x = static_cast<int>(toplevel.scene_tree->node.x);
        state.y = static_cast<int>(toplevel.scene_tree->node.y);
        state.width = toplevel.xdg_toplevel->base->geometry.width;
        state.height = toplevel.xdg_toplevel->base->geometry.height;
        state.row = toplevel.workspace_row;
        state.column = toplevel.workspace_column;
        session->states.set(toplevel.session_name, state);
        save_states(*session);
        return;
    }
}

void on_toplevel_mapped(Toplevel& toplevel)
{
    if (!toplevel.has_session_restore_position)
        return;
    wlr_scene_node_set_position(&toplevel.scene_tree->node, toplevel.session_restore_position.x(), toplevel.session_restore_position.y());
    toplevel.has_session_restore_position = false;
}

void on_toplevel_destroyed(Toplevel& toplevel)
{
    for (auto& session : s_sessions) {
        for (auto& entry : session->toplevels) {
            if (entry->toplevel == &toplevel)
                entry->toplevel = nullptr;
        }
    }
}

static ByteString running_apps_path()
{
    if (auto const* dir = getenv("SDE_STATE_DIR"); dir && *dir)
        return ByteString::formatted("{}/session-apps", dir);
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-session-apps", runtime);
    return ByteString("/tmp/sde-session-apps");
}

void update_running_apps(Server& server)
{
    // One entry per application with a mapped toplevel, in the order they
    // appeared (an app whose windows have all closed is dropped, so the list is
    // always the live set). The session supervisor reads it after a restart and
    // relaunches these applications; the ones that opted into
    // xdg-session-management-v1 then restore their toplevels' geometry.
    Vector<ByteString> apps;
    for (auto& toplevel : server.toplevels) {
        if (toplevel->mapped == false)
            continue;
        auto const* app_id = toplevel->xdg_toplevel->app_id;
        if (app_id == nullptr || *app_id == '\0')
            continue;
        ByteString id { app_id };
        if (!apps.contains_slow(id))
            apps.append(move(id));
    }

    auto path = running_apps_path();
    int fd = ::open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    for (auto const& app : apps) {
        (void)::write(fd, app.characters(), app.length());
        (void)::write(fd, "\n", 1);
    }
    ::close(fd);
}

}
