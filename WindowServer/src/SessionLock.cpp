/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "InputMethod.h"
#include "Keyboard.h"
#include "Output.h"
#include "SessionLock.h"

#include <stdlib.h>
#include <unistd.h>

#include "Wlroots.h"

// --- ext-session-lock-v1 ---------------------------------------------------
// The compositor must cover every output with a configured lock surface before
// telling the client the session is locked, and must not deliver input to normal
// clients while locked.

void spawn_lock_client()
{
    // A standard ext-session-lock-v1 client, so any conformant locker works.
    // swaylock is the default; override with SDE_LOCK_CMD (gtklock, hyprlock, a
    // future SDE-native client, ...).
    char const* command = getenv("SDE_LOCK_CMD");
    if (!command || !*command)
        command = "swaylock";
    dbgln("WindowServer: locking via '{}'", command);
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", command, static_cast<char*>(nullptr));
        _exit(127);
    }
}

static LockSurface* lock_surface_for_output(Server& server, wlr_output* output)
{
    for (auto& lock : server.lock.surfaces) {
        if (lock->lock_surface && lock->lock_surface->output == output && lock->lock_surface->surface)
            return lock.ptr();
    }
    return nullptr;
}

LockSurface* lock_surface_at_cursor(Server& server)
{
    // Prefer the lock surface on the output under the cursor; fall back to any
    // (e.g. a headless/cursorless client).
    auto* output = wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y);
    if (auto* lock = lock_surface_for_output(server, output))
        return lock;
    for (auto& lock : server.lock.surfaces) {
        if (lock->lock_surface && lock->lock_surface->surface)
            return lock.ptr();
    }
    return nullptr;
}

void reconfigure_lock_surface(Server& server, wlr_output* output)
{
    if (!server.lock.locked || !output)
        return;
    auto* lock = lock_surface_for_output(server, output);
    if (!lock)
        return;
    // output.commit fires every frame; only reconfigure on a real size change.
    Gfx::IntSize const wanted = output_logical_size(output);
    if (lock->configured_size == wanted)
        return;
    lock->configured_size = wanted;
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, output, &box);
    dbgln("WindowServer: lock surface re-configured for '{}' {}x{}", output->name ? output->name : "?", wanted.width(), wanted.height());
    if (lock->scene)
        wlr_scene_node_set_position(&lock->scene->buffer->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(lock->lock_surface, static_cast<uint32_t>(wanted.width()), static_cast<uint32_t>(wanted.height()));
}

static void lock_surface_commit(wl_listener* listener, void*)
{
    LockSurface* lock = wl_container_of(listener, lock, commit);
    Server& server = *lock->server;
    if (lock->configured)
        return;
    if (!lock->lock_surface || !lock->lock_surface->surface || !lock->lock_surface->surface->buffer)
        return;

    lock->configured = true;
    dbgln("WindowServer: session lock surface covered an output");

    int covered = 0;
    for (auto& candidate : server.lock.surfaces)
        covered += candidate->configured ? 1 : 0;
    if (server.lock.lock && !server.lock.locked && covered >= static_cast<int>(server.outputs.size())) {
        wlr_session_lock_v1_send_locked(server.lock.lock);
        server.lock.locked = true;
        // R5 IME: no text input on the lock screen.
        InputMethod::set_focus(server, nullptr);
        // Give the keyboard to a lock surface (the primary output's, else any).
        auto* lock = lock_surface_for_output(server, server.primary_output);
        if (!lock)
            lock = lock_surface_at_cursor(server);
        if (lock) {
            if (auto* keyboard = wlr_seat_get_keyboard(server.seat))
                wlr_seat_keyboard_notify_enter(server.seat, lock->lock_surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
        }
        stop_key_repeat();
        dbgln("WindowServer: session locked");
    }
}

static void lock_surface_destroy(wl_listener* listener, void*)
{
    LockSurface* lock = wl_container_of(listener, lock, destroy);
    Server& server = *lock->server;
    wl_list_remove(&lock->commit.link);
    wl_list_remove(&lock->destroy.link);
    if (lock->scene)
        wlr_scene_node_destroy(&lock->scene->buffer->node);
    server.lock.surfaces.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == lock; });
}

static void session_lock_new_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, lock.new_surface);
    auto* lock_surface = static_cast<wlr_session_lock_surface_v1*>(data);
    auto* output = lock_surface->output;
    dbgln("WindowServer: session lock surface for output '{}'", output && output->name ? output->name : "?");

    auto entry = make<LockSurface>();
    entry->server = server;
    entry->lock_surface = lock_surface;
    entry->scene = wlr_scene_surface_create(server->overlay_tree, lock_surface->surface);
    use_nearest_filter(&entry->scene->buffer->node);
    wlr_box box { };
    if (output)
        wlr_output_layout_get_box(server->output_layout, output, &box);
    wlr_scene_node_set_position(&entry->scene->buffer->node, box.x, box.y);
    wlr_scene_node_set_enabled(&entry->scene->buffer->node, true);

    entry->commit.notify = lock_surface_commit;
    wl_signal_add(&lock_surface->surface->events.commit, &entry->commit);
    entry->destroy.notify = lock_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &entry->destroy);

    if (output) {
        auto const wanted = output_logical_size(output);
        entry->configured_size = wanted;
        wlr_session_lock_surface_v1_configure(lock_surface, static_cast<uint32_t>(wanted.width()), static_cast<uint32_t>(wanted.height()));
    }

    server->lock.surfaces.append(move(entry));
}

static void session_lock_unlock(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, lock.unlock);
    dbgln("WindowServer: session unlocked");
    // wlroots destroys the lock surfaces (and our per-surface handlers clean them
    // up) as the client destroys the lock right after this signal.
    server->lock.locked = false;
    stop_key_repeat();
}

static void session_lock_destroy(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, lock.destroy);
    // By now wlroots has destroyed every lock surface (their handlers removed
    // them). Unlink our lock listeners so wlroots' final assertions pass.
    wl_list_remove(&server->lock.new_surface.link);
    wl_list_remove(&server->lock.unlock.link);
    wl_list_remove(&server->lock.destroy.link);
    server->lock.lock = nullptr;
    server->lock.locked = false;
    stop_key_repeat();
}

static void session_lock_new_lock(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, lock.new_lock);
    auto* lock = static_cast<wlr_session_lock_v1*>(data);
    if (server->lock.lock) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    dbgln("WindowServer: session lock requested");
    server->lock.lock = lock;
    server->lock.new_surface.notify = session_lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->lock.new_surface);
    server->lock.unlock.notify = session_lock_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock.unlock);
    server->lock.destroy.notify = session_lock_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock.destroy);
}

void initialize_session_lock(Server& server)
{
    server.lock.manager = wlr_session_lock_manager_v1_create(server.display);
    server.lock.new_lock.notify = session_lock_new_lock;
    wl_signal_add(&server.lock.manager->events.new_lock, &server.lock.new_lock);
}
