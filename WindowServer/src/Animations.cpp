/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Animations.h"
#include "LayerShell.h"
#include "WindowManager.h"

#include <AK/StdLibExtras.h>
#include <LibCore/Timer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>

#include "Wlroots.h"

// --- window animations ----------------------------------------------------
//
// SerenityOS animates minimize/restore as an inverted outline that interpolates
// between the window's frame and its Taskbar button (Window::start_minimize_
// animation). We draw the same outline (a ChromeNode in the overlay tree) over
// 150 ms, gated on [Effects] AnimateWindows, using the button rect the Taskbar
// reports over the WindowManagerServer (set_window_taskbar_rect).
// A short outline drawn over the desktop: minimize/restore interpolate between
// the window and its Taskbar button, and a launch grows the outline out of the
// rect the window was launched from (SerenityOS's Window animations).
struct OutlineAnimation {
    Gfx::IntRect from;
    Gfx::IntRect to;
    Gfx::Color color { Gfx::Color::Black };
    i64 start_ms { 0 };
    OwnPtr<ChromeNode> node;
};

static Vector<OutlineAnimation> s_outline_animations;

static RefPtr<Core::Timer> s_animation_timer;

static constexpr int outline_animation_duration_ms = 150;

static Core::ElapsedTimer& animation_clock()
{
    static Core::ElapsedTimer clock = Core::ElapsedTimer::start_new();
    return clock;
}

static Gfx::IntRect interpolate_rect(Gfx::IntRect const& a, Gfx::IntRect const& b, float t)
{
    auto lerp = [t](int from, int to) {
        return from + static_cast<int>(static_cast<float>(to - from) * t + (t >= 0.5f ? 0.5f : -0.5f));
    };
    return { lerp(a.x(), b.x()), lerp(a.y(), b.y()), lerp(a.width(), b.width()), lerp(a.height(), b.height()) };
}

static void tick_outline_animations()
{
    i64 const now = animation_clock().elapsed_milliseconds();
    bool any_active = false;
    for (auto& animation : s_outline_animations) {
        float t = clamp(static_cast<float>(now - animation.start_ms) / outline_animation_duration_ms, 0.0f, 1.0f);
        float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
        auto rect = interpolate_rect(animation.from, animation.to, eased);
        if (animation.node && rect.width() > 0 && rect.height() > 0) {
            animation.node->set_visible(true);
            animation.node->set_rect(rect);
            animation.node->render([&](Gfx::Painter& painter) {
                painter.draw_rect_with_thickness({ 0, 0, rect.width(), rect.height() }, animation.color, 2);
            });
        }
        if (t < 1.0f)
            any_active = true;
    }
    if (!any_active) {
        s_outline_animations.clear(); // destroys the ChromeNodes (hides them)
        if (s_animation_timer)
            s_animation_timer->stop();
    }
}

static void start_outline_animation(Server& server, Gfx::IntRect const& from, Gfx::IntRect const& to, Gfx::Color color)
{
    if (!server.overlay_tree || from.is_empty() || to.is_empty())
        return;

    OutlineAnimation animation;
    animation.from = from;
    animation.to = to;
    animation.color = color;
    animation.start_ms = animation_clock().elapsed_milliseconds();
    animation.node = make<ChromeNode>(*server.overlay_tree);
    animation.node->set_rect(from);
    s_outline_animations.append(move(animation));

    if (!s_animation_timer)
        s_animation_timer = Core::Timer::create_repeating(15, tick_outline_animations);
    s_animation_timer->start();
}

void start_minimize_animation(Server& server, Toplevel& toplevel, Gfx::IntRect const& window_rect, bool minimizing)
{
    if (!Config::effects().animate_windows || window_rect.is_empty())
        return;

    Gfx::IntRect target = toplevel.taskbar_rect;
    if (target.is_empty()) {
        // No Taskbar button (no Taskbar, or one that has not reported yet): use
        // the bottom-right corner (the applet/clock end of the Taskbar).
        auto* output_state = output_for(server, server.primary_output);
        Gfx::IntRect area { 0, 0, 1280, 720 };
        if (output_state && !wlr_box_empty(&output_state->usable_area))
            area = { output_state->usable_area.x, output_state->usable_area.y, output_state->usable_area.width, output_state->usable_area.height };
        target = { area.right() - 24, area.bottom() - 24, 4, 4 };
    }

    auto from = minimizing ? window_rect : target;
    auto to = minimizing ? target : window_rect;
    start_outline_animation(server, from, to, g_palette ? g_palette->selection() : Gfx::Color::Black);
    dbgln("WindowServer: {} animation {}x{}+{},{} -> {}x{}+{},{}",
        minimizing ? "minimize" : "restore",
        from.width(), from.height(), from.x(), from.y(),
        to.width(), to.height(), to.x(), to.y());
}

// SerenityOS Window::start_launch_animation(): the window grows out of the rect
// the launcher handed the application (the launcher sets
// __libgui_launch_origin_rect, GUI::Window forwards it over
// serenity_toplevel.set_launch_origin).
void start_launch_animation(Server& server, Toplevel& toplevel)
{
    if (toplevel.launch_origin.is_empty() || !Config::effects().animate_windows)
        return;
    // Like SerenityOS, only animate a window that appears on the visible stack.
    if (toplevel.workspace_row != WindowManager::current_workspace_row() || toplevel.workspace_column != WindowManager::current_workspace_column())
        return;

    Gfx::IntRect frame {
        toplevel.scene_tree->node.x, toplevel.scene_tree->node.y,
        toplevel.frame_size.width(), toplevel.frame_size.height()
    };
    if (frame.is_empty())
        frame = { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y, toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };
    if (frame.is_empty())
        return;

    start_outline_animation(server, toplevel.launch_origin, frame, g_palette ? g_palette->selection() : Gfx::Color::Black);
    dbgln("WindowServer: launch animation {}x{}+{},{} -> {}x{}+{},{}",
        toplevel.launch_origin.width(), toplevel.launch_origin.height(), toplevel.launch_origin.x(), toplevel.launch_origin.y(),
        frame.width(), frame.height(), frame.x(), frame.y());
}
