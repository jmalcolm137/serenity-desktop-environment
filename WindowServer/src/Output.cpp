/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "InputCapture.h"
#include "LayerShell.h"
#include "Output.h"
#include "OutputManagement.h"
#include "Performance.h"
#include "SessionLock.h"
#include "Wallpaper.h"
#include "XdgShell.h"

#include <ctime>
#include <stdlib.h>

#include "Wlroots.h"

static void reassign_primary_output(Server& server, wlr_output* gone);

// --- outputs --------------------------------------------------------------

// Compose the desktop background for an output: the theme's background colour,
// optionally overlaid with a wallpaper image. Rebuilt only when the output size
// changes.
// Logical size of an output (physical resolution divided by scale). The scene is
// laid out in logical coordinates, so anything we size or place ourselves must use
// this, not wlr_output->width/height (which are physical pixels).
Gfx::IntSize output_logical_size(wlr_output* output)
{
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(output, &width, &height);
    return { width, height };
}

// Nearest filtering makes an integer upscale crisp (pixel-doubling) instead of
// bilinear-soft, and is a no-op at 1x. Applied to the app content and compositor
// chrome buffers so the HiDPI stopgap looks sharp.
static void set_buffer_filter(struct wlr_scene_buffer* buffer, int, int, void*)
{
    // An integer scale is exact, so nearest-neighbour is a pixel-doubling no-op at
    // 1x and crisp at 2x; a fractional scale needs linear filtering.
    wlr_scene_buffer_set_filter_mode(buffer, output_scale_is_integral() ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR);
}

void use_nearest_filter(struct wlr_scene_node* node)
{
    wlr_scene_node_for_each_buffer(node, set_buffer_filter, nullptr);
}

// R6 presentation-time: mark the surfaces the upcoming frame displays on this
// output, so wlroots can deliver wp_presentation_feedback ("presented" events).
// The scene reports the buffers visible on the output in rendering order.
static void mark_buffer_presented(wlr_scene_buffer* buffer, int sx, int sy, void* data)
{
    (void)sx;
    (void)sy;
    auto* output = static_cast<wlr_output*>(data);
    if (auto* scene_surface = wlr_scene_surface_try_from_buffer(buffer))
        wlr_presentation_surface_textured_on_output(scene_surface->surface, output);
}

static void output_frame(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, frame);
    double const frame_start_ms = perf_now_ms();
    update_desktop_background(*output->server, *output);
    auto* scene_output = wlr_scene_get_scene_output(output->server->scene, output->output);
    if (!scene_output)
        return;
    wlr_scene_output_for_each_buffer(scene_output, mark_buffer_presented, output->output);
    wlr_scene_output_commit(scene_output, nullptr);
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);

    // The budget is one refresh interval of the output we are rendering on, so a
    // 144 Hz or 240 Hz panel is held to its own cadence (and a 60 Hz panel to
    // 16.667 ms) without hardcoding a refresh rate.
    s_perf.refresh_mhz = output->output->refresh;
    s_perf.frame_ms.budget_ms = perf_frame_budget_ms(s_perf.refresh_mhz);

    double const frame_end_ms = perf_now_ms();
    s_perf.frame_ms.add(frame_end_ms - frame_start_ms);
    if (s_perf_last_frame_ms > 0)
        s_perf.frame_interval_ms.add(frame_start_ms - s_perf_last_frame_ms);
    s_perf_last_frame_ms = frame_start_ms;
    if (s_perf_pending_input_ms >= 0) {
        s_perf.input_to_present_ms.add(frame_end_ms - s_perf_pending_input_ms);
        s_perf_pending_input_ms = -1;
    }
    ++s_perf.frames;
    if (frame_end_ms - frame_start_ms > s_perf.frame_ms.budget_ms)
        ++s_perf.slow_frames;
}

static void output_request_state(wl_listener* listener, void* data)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, request_state);
    auto* event = static_cast<wlr_output_event_request_state*>(data);
    wlr_output_commit_state(output->output, event->state);
}

// R6/N5 (multi-output/hotplug). An output that goes away — unplugged, or disabled
// through zwlr_output_management_v1 — must not take its windows with it. Window
// positions are layout coordinates, so anything that was on the vanished output
// ends up outside every remaining one: invisible, and unreachable without editing
// the layout by hand. Move those windows onto a surviving output, keeping their
// offset from the old output's origin, so "the window at the top-left" stays the
// window at the top-left.
// The user's choice of main screen (`MainScreen` in screenlayout.ini, which
// Display Settings writes). The primary output is what the workarea, window
// placement and the X11 workarea come from, so the two should agree — before this
// the primary was simply the first output the backend created and the setting was
// ignored.
static wlr_output* configured_main_output(Server& server, wlr_output* excluding)
{
    auto const& outputs = Config::outputs();
    if (outputs.is_empty())
        return nullptr;
    unsigned const index = min(Config::main_screen_index(), outputs.size() - 1);
    auto const& wanted = outputs[index].device;
    if (wanted.is_empty())
        return nullptr;
    for (auto& output : server.outputs) {
        if (output->output == excluding || !output->output->enabled || !output->output->name)
            continue;
        if (wanted == output->output->name)
            return output->output;
    }
    return nullptr;
}

// An output appearing — or coming back after a replug — is where the configured
// main screen gets its primacy (the other half of reassign_primary_output).
static void maybe_restore_primary_output(Server& server)
{
    auto* wanted = configured_main_output(server, nullptr);
    if (!wanted || server.primary_output == wanted)
        return;
    server.primary_output = wanted;
    dbgln("WindowServer: primary output is now '{}' (the configured main screen)",
        wanted->name ? wanted->name : "?");
}

static wlr_output* first_enabled_output(Server& server, wlr_output* excluding)
{
    for (auto& output : server.outputs) {
        if (output->output == excluding || !output->output->enabled)
            continue;
        return output->output;
    }
    return nullptr;
}

static void migrate_toplevels_off_output(Server& server, wlr_box const& gone, wlr_output* replacement)
{
    if (wlr_box_empty(&gone) || !replacement)
        return;
    wlr_box target { };
    wlr_output_layout_get_box(server.output_layout, replacement, &target);
    if (wlr_box_empty(&target))
        return;

    auto move_if_on_gone_output = [&](wlr_scene_tree* scene_tree) {
        auto& node = scene_tree->node;
        if (node.x < gone.x || node.y < gone.y
            || node.x >= gone.x + gone.width || node.y >= gone.y + gone.height)
            return false;
        int x = target.x + (node.x - gone.x);
        int y = target.y + (node.y - gone.y);
        // Keep the origin on the surviving output: a window larger than it (the
        // default size of a client that asked for the whole screen) stays reachable.
        x = max(target.x, min(x, target.x + max(0, target.width - 1)));
        y = max(target.y, min(y, target.y + max(0, target.height - 1)));
        wlr_scene_node_set_position(&node, x, y);
        return true;
    };

    unsigned moved = 0;
    unsigned refitted = 0;
    for (auto& toplevel : server.toplevels) {
        if (!toplevel->scene_tree || toplevel->minimized)
            continue;
        if (!move_if_on_gone_output(toplevel->scene_tree))
            continue;
        ++moved;
        // A maximised window's size *is* the old output's workarea, so moving the
        // origin alone would leave it the wrong size for the output it is now on.
        if (toplevel->maximized) {
            toplevel_maximize_on_primary(*toplevel);
            ++refitted;
        }
    }
    for (auto& toplevel : server.xwayland.toplevels) {
        if (!toplevel->scene_tree || !move_if_on_gone_output(toplevel->scene_tree))
            continue;
        ++moved;
        if (toplevel->maximized) {
            // An X11 client fills the workarea we report, so re-ask it now that the
            // primary — and with it _NET_WORKAREA — has moved.
            wlr_xwayland_surface_set_maximized(toplevel->xsurface, true, true);
            ++refitted;
        }
    }
    if (moved > 0) {
        dbgln("WindowServer: moved {} window(s) off the removed output onto '{}' ({} re-fitted)",
            moved, replacement->name ? replacement->name : "?", refitted);
    }
}

// Everything that has to happen when an output is gone: its windows move to the
// primary (or whatever is left), the primary is reassigned if it was the lost one,
// and maximised windows that moved are re-fitted to their new output. Order
// matters — the re-fit maximises onto the primary, so the primary has to be the new
// one by then.
static void handle_output_lost(Server& server, wlr_box const& gone, wlr_output* lost)
{
    reassign_primary_output(server, lost);
    wlr_output* target = (server.primary_output && server.primary_output != lost)
        ? server.primary_output
        : first_enabled_output(server, lost);
    migrate_toplevels_off_output(server, gone, target);
}

// The primary output is the reference for the workarea and for placing new
// windows. Losing it must reassign it — every placement afterwards would
// otherwise use a destroyed output.
static void reassign_primary_output(Server& server, wlr_output* gone)
{
    if (server.primary_output != gone)
        return;
    // Prefer the configured main screen if it is still available; otherwise take
    // whatever is left, so the primary is never a destroyed output.
    server.primary_output = configured_main_output(server, gone);
    if (!server.primary_output)
        server.primary_output = first_enabled_output(server, gone);
    dbgln("WindowServer: primary output is now '{}'",
        server.primary_output && server.primary_output->name ? server.primary_output->name : "(none)");
}

static void output_destroy(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, destroy);
    auto& server = *output->server;
    // Take its windows and its primacy with us before the output is forgotten...
    wlr_box const gone = output->area;
    handle_output_lost(server, gone, output->output);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->commit.link);
    if (output->background)
        wlr_scene_node_destroy(&output->background->node);
    remove_owned(server.outputs, output);
    // R5: the InputCapture zones are the outputs.
    InputCapture::on_outputs_changed(server);
}

static void output_commit(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, commit);
    auto& server = *output->server;
    // Read the new box, but keep the old one: a disable empties it, and the windows
    // are still where they were, so the migration below needs to know where the
    // output *was*. (Getting this order wrong is how the first version of this
    // silently moved nothing.)
    wlr_box const previous_area = output->area;
    wlr_output_layout_get_box(server.output_layout, output->output, &output->area);

    // Disabling an output (zwlr_output_management_v1) is the hotplug path a test
    // can drive: the output stays, but nothing of it is on screen any more, so its
    // windows have to move and the primary may have to be reassigned.
    bool const enabled = output->output->enabled;
    if (output->was_enabled && !enabled) {
        dbgln("WindowServer: output '{}' disabled", output->output->name ? output->output->name : "?");
        wlr_box const gone = wlr_box_empty(&output->area) ? previous_area : output->area;
        handle_output_lost(server, gone, output->output);
    } else if (!output->was_enabled && enabled) {
        // Back after a replug: if it is the configured main screen, it is primary
        // again. Serenity's MainScreen index is the user's choice; without this a
        // replugged monitor kept whatever primacy it had lost.
        maybe_restore_primary_output(server);
    }
    output->was_enabled = enabled;
    reconfigure_lock_surface(*output->server, output->output);
}

// --- HiDPI / fractional output scale ---------------------------------------
//
// Serenity's LibGUI renders on the CPU and its fonts are fixed bitmaps, so a
// high-DPI panel is made usable by scaling the output (integer by default, but
// fractional is supported: the compositor renders its own chrome at an integer
// supersample and lets the scene scale it down, native clients learn the exact
// scale from wp-fractional-scale-v1). The client-side backing-store scale is the
// libgui-wayland shim's job (it rasters at the output scale). See
// docs/13-native-session-and-aur-packaging.md.
//
// Auto rule: 2x when the panel's true DPI is >= 144, else 1x; panels that omit
// their physical size fall back to a resolution threshold.
static float auto_output_scale(struct wlr_output* output, struct wlr_output_mode* mode)
{
    int const width = mode ? mode->width : output->width;
    int const height = mode ? mode->height : output->height;

    // Prefer the panel's physical size (true DPI) when it reports one...
    if (output->phys_width > 0) {
        double const dpi = width / (static_cast<double>(output->phys_width) / 25.4);
        return dpi >= 144.0 ? 2.0f : 1.0f;
    }
    // ...but eDP panels often omit it, so fall back to a resolution threshold
    // that keeps 1080p/1440p displays at 1x while catching 1600p/1800p/4K panels.
    return (width >= 2560 && height >= 1600) ? 2.0f : 1.0f;
}

// Resolve the output refresh (mHz; 0 = let the backend choose): the
// SDE_OUTPUT_REFRESH override wins (the counterpart of SDE_OUTPUT_SCALE), then a
// saved layout entry. A *virtual* output carries no refresh in its modes — the
// headless backend defaults to 60 Hz — so this is also how such an output is held
// to a given cadence (the performance budget test runs one at 240 Hz).
int resolve_output_refresh(Config::OutputSettings const* saved)
{
    if (auto const* env = getenv("SDE_OUTPUT_REFRESH"); env && *env) {
        int const hz = atoi(env);
        if (hz > 0)
            return hz * 1000;
    }
    (void)saved;
    return 0;
}

// Resolve the output scale: the SDE_OUTPUT_SCALE override wins, then the
// configured [Output] DefaultScale, then a saved layout, then the auto rule.
float resolve_output_scale(struct wlr_output* output, struct wlr_output_mode* mode, Config::OutputSettings const* saved)
{
    if (auto const* env = getenv("SDE_OUTPUT_SCALE"); env && *env) {
        char* end = nullptr;
        double const value = strtod(env, &end);
        if (end != env && value >= 0.5)
            return static_cast<float>(value);
    }
    if (double const configured = Config::output().default_scale; configured > 0)
        return static_cast<float>(configured);
    if (saved && saved->scale > 0)
        return static_cast<float>(saved->scale);
    return auto_output_scale(output, mode);
}

static void server_new_output(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_output);
    auto* wlr_output = static_cast<struct wlr_output*>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // R4.5: honor a saved screen entry (mode/scale) for this output.
    Config::OutputSettings const* saved = nullptr;
    if (wlr_output->name) {
        for (auto const& settings : Config::outputs()) {
            if (settings.mode == "Device"sv && settings.device == wlr_output->name) {
                saved = &settings;
                break;
            }
        }
    }

    if (saved) {
        // One helper for "apply this entry to this output", shared with the runtime
        // path (Config::watch_outputs -> apply_loaded_screen_layout), so a layout
        // change behaves exactly like a replug.
        apply_saved_settings_to_output(*server, wlr_output, *saved);
    } else {
        // No entry: a virtual output (the headless backend) has no mode list, so
        // re-apply its current size, which is how a requested refresh is applied.
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        int const refresh_mhz = resolve_output_refresh(nullptr);
        int width = wlr_output->width;
        int height = wlr_output->height;
        if (auto* preferred = wlr_output_preferred_mode(wlr_output)) {
            width = preferred->width;
            height = preferred->height;
        }
        if (refresh_mhz > 0 && width > 0 && height > 0)
            wlr_output_state_set_custom_mode(&state, width, height, refresh_mhz);
        else if (auto* preferred = wlr_output_preferred_mode(wlr_output))
            wlr_output_state_set_mode(&state, preferred);
        wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);
    }
    // Compositor-drawn chrome (frames, overlays, the switcher) is rendered at the
    // output scale so it stays crisp on HiDPI panels.
    set_output_scale(wlr_output->scale);
    auto output = make<Output>();
    output->server = server;
    output->output = wlr_output;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    output->commit.notify = output_commit;
    wl_signal_add(&wlr_output->events.commit, &output->commit);

    auto* layout_output = saved
        ? wlr_output_layout_add(server->output_layout, wlr_output, saved->left, saved->top)
        : wlr_output_layout_add_auto(server->output_layout, wlr_output);
    auto* scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

    if (!server->primary_output)
        server->primary_output = wlr_output;
    maybe_restore_primary_output(*server);

    auto const logical_size = output_logical_size(wlr_output);
    dbgln("WindowServer: output '{}' {}x{} @{}x", wlr_output->name ? wlr_output->name : "?", logical_size.width(), logical_size.height(), wlr_output->scale);
    server->outputs.append(move(output));
    update_desktop_background(*server, *server->outputs.last());
    arrange_layers(*server, wlr_output);
    output_manager_send_configuration(*server);
    // R4.5: keep screenlayout.ini in sync with the live layout (coalesced so a
    // startup burst of outputs writes the complete layout once).
    schedule_persist_screen_layout(*server);
    // R5: the InputCapture zones are the outputs.
    InputCapture::on_outputs_changed(*server);
}

void initialize_outputs(Server& server)
{
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);
}
