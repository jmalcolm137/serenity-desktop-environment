/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Idle.h"
#include "Performance.h"

#include "Wlroots.h"

// --- session safety net (R4.1) --------------------------------------------
// Idle notification (ext-idle-notify-v1), idle inhibition (zwp_idle_inhibit_v1)
// and DPMS (wlr-output-power-management). See docs/11-r4-implementation-plan.md.

void notify_activity(Server& server)
{
    // R6: this is also where we mark the start of the input -> present interval.
    perf_note_input();
    if (server.idle.notifier)
        wlr_idle_notifier_v1_notify_activity(server.idle.notifier, server.seat);
}

static void update_idle_inhibit(Server& server, wlr_idle_inhibitor_v1* being_destroyed = nullptr)
{
    if (!server.idle.notifier || !server.idle.inhibit_manager)
        return;
    // wlroots removes an inhibitor from the manager's list *after* emitting its
    // destroy signal, so when we are called from that signal the dying inhibitor
    // is still linked; exclude it or we would never clear the inhibited state.
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor = nullptr;
    wl_list_for_each(inhibitor, &server.idle.inhibit_manager->inhibitors, link)
    {
        if (inhibitor == being_destroyed)
            continue;
        inhibited = true;
        break;
    }
    wlr_idle_notifier_v1_set_inhibited(server.idle.notifier, inhibited);
}

static void idle_inhibitor_destroy(wl_listener* listener, void*)
{
    IdleInhibitor* inhibitor = wl_container_of(listener, inhibitor, destroy);
    auto& server = *inhibitor->server;
    dbgln("WindowServer: idle inhibitor destroyed");
    // Unlink before freeing the entry: wlroots asserts the inhibitor's destroy
    // listener list is empty before it frees the object.
    wl_list_remove(&inhibitor->destroy.link);
    auto* dying = inhibitor->inhibitor;
    server.idle.inhibitors.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == inhibitor; });
    update_idle_inhibit(server, dying);
}

static void server_new_idle_inhibitor(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, idle.new_inhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    dbgln("WindowServer: idle inhibitor created");

    auto entry = make<IdleInhibitor>();
    entry->server = server;
    entry->inhibitor = inhibitor;
    entry->destroy.notify = idle_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &entry->destroy);
    server->idle.inhibitors.append(move(entry));
    update_idle_inhibit(*server);
}

static void set_output_power(wlr_output* output, bool on)
{
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    if (!wlr_output_commit_state(output, &state)) {
        dbgln("WindowServer: failed to turn output '{}' {}", output->name ? output->name : "?", on ? "on" : "off");
    } else {
        dbgln("WindowServer: output '{}' {}", output->name ? output->name : "?", on ? "on" : "off");
    }
    wlr_output_state_finish(&state);
}

static void output_power_set_mode(wl_listener* listener, void* data)
{
    (void)listener;
    auto* event = static_cast<wlr_output_power_v1_set_mode_event*>(data);
    set_output_power(event->output, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

void initialize_idle(Server& server)
{
    // Session safety net (R4.1): idle notification, idle inhibition and DPMS.
    server.idle.notifier = wlr_idle_notifier_v1_create(server.display);
    server.idle.inhibit_manager = wlr_idle_inhibit_v1_create(server.display);
    server.idle.new_inhibitor.notify = server_new_idle_inhibitor;
    wl_signal_add(&server.idle.inhibit_manager->events.new_inhibitor, &server.idle.new_inhibitor);
    server.idle.output_power_manager = wlr_output_power_manager_v1_create(server.display);
    server.idle.output_power_set_mode.notify = output_power_set_mode;
    wl_signal_add(&server.idle.output_power_manager->events.set_mode, &server.idle.output_power_set_mode);
}

void finish_idle(Server& server)
{
    remove_listener(server.idle.new_inhibitor);
    remove_listener(server.idle.output_power_set_mode);
}
