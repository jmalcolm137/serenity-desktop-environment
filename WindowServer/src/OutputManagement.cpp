/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "LayerShell.h"
#include "Output.h"
#include "OutputManagement.h"
#include "Wallpaper.h"

#include <LibCore/Timer.h>

#include <stdlib.h>

#include "Wlroots.h"

// --- output management ----------------------------------------------------

// R4.5: persist the live output layout to screenlayout.ini so it survives a
// restart and is visible to DisplaySettings (via the port's get_screen_layout).
// True if the live layout already matches what screenlayout.ini holds (matched by
// device), so persisting would be a no-op. Skipping avoids pointless writes that
// race with a live edit, re-trigger the outputs watcher, and (during startup) could
// clobber the saved entries.
static bool live_layout_matches_loaded(Vector<Config::OutputSettings> const& computed)
{
    auto const& loaded = Config::outputs();
    if (computed.size() != loaded.size())
        return false;
    for (auto const& entry : computed) {
        bool found = false;
        for (auto const& existing : loaded) {
            if (existing.device == entry.device && existing.mode == entry.mode
                && existing.left == entry.left && existing.top == entry.top
                && existing.width == entry.width && existing.height == entry.height
                && existing.scale == entry.scale) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void persist_screen_layout(Server& server)
{
    Vector<Config::OutputSettings> settings;
    unsigned main_index = 0;
    for (size_t index = 0; index < server.outputs.size(); ++index) {
        auto& output = *server.outputs[index];
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output.output, &box);
        Config::OutputSettings entry;
        entry.mode = "Device"sv;
        entry.device = output.output->name ? output.output->name : "sde-0";
        entry.left = box.x;
        entry.top = box.y;
        entry.width = output.output->width;
        entry.height = output.output->height;
        // The SerenityOS screenlayout.ini ScaleFactor is an integer; a fractional
        // scale is recorded as 0 ("unspecified") so a reload re-decides it from
        // [Output] DefaultScale / the auto rule instead of a wrong integer.
        entry.scale = output_scale_is_integral() ? static_cast<int>(output.output->scale) : 0;
        settings.append(move(entry));
        if (output.output == server.primary_output)
            main_index = index;
    }
    if (live_layout_matches_loaded(settings))
        return;
    Config::set_outputs(move(settings), main_index);
}

// Persist the layout, coalesced. Outputs are created in a burst at startup (and a
// config edit can re-scale several at once); writing after each would clobber the
// saved entries for outputs not yet present, and the outputs-file watcher would
// then reload the truncated file and place the remaining outputs with add_auto
// (the multi-output position bug). One short timer writes the settled layout once.
void schedule_persist_screen_layout(Server& server)
{
    static RefPtr<Core::Timer> timer;
    static Server* target = nullptr;
    target = &server;
    timer = Core::Timer::create_single_shot(120, [] {
        if (target)
            persist_screen_layout(*target);
    });
    timer->start();
}

// R4.5: apply a saved screen entry (mode/scale/position) to a live output.
void apply_saved_settings_to_output(Server& server, struct wlr_output* output, Config::OutputSettings const& settings)
{
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode* mode = nullptr;
    if (settings.width > 0 && settings.height > 0) {
        struct wlr_output_mode* candidate;
        wl_list_for_each(candidate, &output->modes, link)
        {
            if (candidate->width == settings.width && candidate->height == settings.height) {
                mode = candidate;
                break;
            }
        }
    }
    // Keep the output's current refresh unless one is configured: re-applying a
    // saved layout must not silently drop a custom mode back to the backend's
    // default (a virtual output carries no refresh in its modes).
    int const refresh_mhz = resolve_output_refresh(&settings);
    int const refresh = refresh_mhz > 0 ? refresh_mhz : output->refresh;
    if (mode && refresh_mhz == 0)
        wlr_output_state_set_mode(&state, mode);
    else if (settings.width > 0 && settings.height > 0)
        wlr_output_state_set_custom_mode(&state, settings.width, settings.height, refresh);
    else if (auto* preferred = wlr_output_preferred_mode(output)) {
        if (refresh > 0)
            wlr_output_state_set_custom_mode(&state, preferred->width, preferred->height, refresh);
        else
            wlr_output_state_set_mode(&state, preferred);
    }
    // The resolved scale honours SDE_OUTPUT_SCALE / [Output] DefaultScale first, so
    // a configured scale wins over the persisted (integer) layout.
    float const scale = resolve_output_scale(output, mode, &settings);
    wlr_output_state_set_scale(&state, scale);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    wlr_output_layout_add(server.output_layout, output, settings.left, settings.top);
    dbgln("WindowServer: applied screen layout to '{}': {}x{} @{}x at {},{}",
        output->name ? output->name : "?", settings.width, settings.height, scale, settings.left, settings.top);
}

// R6 HiDPI: re-apply the resolved output scale to the live outputs. Called at
// startup and whenever [Output] DefaultScale changes so a config edit takes effect
// immediately (Live reload, docs/14 §4).
void apply_output_scale(Server& server)
{
    bool changed = false;
    for (auto& output : server.outputs) {
        Config::OutputSettings const* saved = nullptr;
        for (auto const& settings : Config::outputs()) {
            if (output->output->name && settings.mode == "Device"sv && settings.device == output->output->name) {
                saved = &settings;
                break;
            }
        }
        float const scale = resolve_output_scale(output->output, wlr_output_preferred_mode(output->output), saved);
        if (output->output->scale == scale)
            continue;

        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        // Keep the current mode; only the scale changes.
        wlr_output_state_set_scale(&state, scale);
        if (wlr_output_commit_state(output->output, &state)) {
            set_output_scale(output->output->scale);
            changed = true;
            dbgln("WindowServer: output '{}' re-scaled to {}x", output->output->name ? output->output->name : "?", output->output->scale);
            update_desktop_background(server, *output);
            arrange_layers(server, output->output);
        }
        wlr_output_state_finish(&state);
    }
    if (changed) {
        output_manager_send_configuration(server);
        schedule_persist_screen_layout(server);
    }
}

// R4.5: re-apply the on-disk layout to the live outputs (screenlayout.ini watch).
void apply_loaded_screen_layout(Server& server)
{
    bool changed = false;
    for (auto const& settings : Config::outputs()) {
        if (settings.mode != "Device"sv || settings.device.is_empty())
            continue;
        for (auto& output : server.outputs) {
            if (output->output->name && settings.device == output->output->name) {
                apply_saved_settings_to_output(server, output->output, settings);
                changed = true;
                break;
            }
        }
    }
    if (changed)
        output_manager_send_configuration(server);
}

void output_manager_send_configuration(Server& server)
{
    if (!server.output_manager)
        return;

    auto* config = wlr_output_configuration_v1_create();
    for (auto& output : server.outputs) {
        auto* head = wlr_output_configuration_head_v1_create(config, output->output);
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output->output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }
    wlr_output_manager_v1_set_configuration(server.output_manager, config);
}

static void output_manager_apply_or_test(Server& server, wlr_output_configuration_v1* config, bool test)
{
    size_t states_len = 0;
    auto* states = wlr_output_configuration_v1_build_state(config, &states_len);
    bool ok = states != nullptr
        && (test ? wlr_backend_test(server.backend, states, states_len)
                 : wlr_backend_commit(server.backend, states, states_len));
    free(states);

    if (!ok) {
        // Say something: a refused configuration is otherwise silent on both sides
        // (the protocol's failed event carries no reason), which makes "Display
        // Settings did nothing" impossible to diagnose.
        dbgln("WindowServer: output configuration refused ({} head(s), {})",
            wl_list_length(&config->heads), test ? "test" : "apply");
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
    }

    if (!test) {
        wlr_output_configuration_head_v1* head = nullptr;
        wl_list_for_each(head, &config->heads, link)
        {
            if (head->state.enabled)
                wlr_output_layout_add(server.output_layout, head->state.output, head->state.x, head->state.y);
        }
    }

    wlr_output_configuration_v1_send_succeeded(config);
    wlr_output_configuration_v1_destroy(config);

    if (!test) {
        output_manager_send_configuration(server);
        // R4.5: a monitor-layout change via zwlr_output_management_v1 is persisted.
        schedule_persist_screen_layout(server);
    }
}

void output_manager_apply(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_apply);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), false);
}

void output_manager_test(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_test);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), true);
}
