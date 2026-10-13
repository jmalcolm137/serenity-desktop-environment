/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WifiIcon.h"

#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>

#include <math.h>

namespace WifiIcon {

// The Breeze 22x22 design space: the geometry of the "network-wireless-*"
// SVGs under breeze/status/22, which is what plasma-nm shows.
static constexpr float design_size = 22.0f;
static constexpr float center_x = 11.0f;
static constexpr float center_y = 17.0f;
static constexpr float dot_outer_radius = 2.0f;
static constexpr float dot_inner_radius = 1.0f;
// Arc bands, outermost first; each band is one unit wide ([r-1, r]).
static constexpr float arc_outer_radii[3] = { 14.0f, 10.0f, 6.0f };
// The half-angle of the fan (from the outer arc endpoints), ~47.8 degrees.
static constexpr float fan_half_angle = 0.8344f;

// The faint opacity Breeze uses for the inactive arcs (network-wireless-N) and
// for the whole fan on the "off" glyph.
static constexpr float inactive_opacity = 0.35f;
static constexpr float disconnected_opacity = 0.5f;

static constexpr Gfx::Color breeze_negative = Gfx::Color::from_rgb(0xda4453);
static constexpr Gfx::Color breeze_neutral = Gfx::Color::from_rgb(0xfdbc4b);

int level_for_strength(int strength)
{
    if (strength == 0)
        return 0;
    if (strength < 20)
        return 20;
    if (strength < 40)
        return 40;
    if (strength < 60)
        return 60;
    if (strength < 80)
        return 80;
    return 100;
}

static float distance_from_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float const abx = bx - ax;
    float const aby = by - ay;
    float const length_squared = abx * abx + aby * aby;
    if (length_squared <= 0.0f)
        return hypotf(px - ax, py - ay);
    float const t = clamp(((px - ax) * abx + (py - ay) * aby) / length_squared, 0.0f, 1.0f);
    return hypotf(px - (ax + t * abx), py - (ay + t * aby));
}

// Rasterises a shape defined by `inside` (design units, relative to the glyph
// centre) with 3x3 supersampling, so the arcs and diagonals stay smooth at any
// size. `base` is the glyph colour; `opacity` scales its alpha.
template<typename InsideFunction>
static void rasterize(Gfx::Painter& painter, Gfx::IntRect const& rect, Gfx::Color base, float opacity, InsideFunction inside)
{
    if (rect.is_empty() || opacity <= 0.0f)
        return;

    float const scale_x = design_size / static_cast<float>(rect.width());
    float const scale_y = design_size / static_cast<float>(rect.height());
    constexpr int samples = 3;
    constexpr float inv_samples = 1.0f / samples;

    painter.add_clip_rect(rect);
    for (int py = 0; py < rect.height(); ++py) {
        for (int px = 0; px < rect.width(); ++px) {
            int hits = 0;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    float const ux = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) * inv_samples) * scale_x - center_x;
                    float const uy = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) * inv_samples) * scale_y - center_y;
                    if (inside(ux, uy))
                        ++hits;
                }
            }
            if (hits == 0)
                continue;
            float const coverage = static_cast<float>(hits) / (samples * samples);
            u8 const alpha = static_cast<u8>(clamp(static_cast<int>(base.alpha() * opacity * coverage + 0.5f), 0, 255));
            if (alpha == 0)
                continue;
            painter.fill_rect(Gfx::IntRect { rect.x() + px, rect.y() + py, 1, 1 }, base.with_alpha(alpha));
        }
    }
    painter.clear_clip_rect();
}

// A stroked segment between two design-space points, used for the X and slash.
static void rasterize_segment(Gfx::Painter& painter, Gfx::IntRect const& rect, float ax, float ay, float bx, float by, float width, Gfx::Color color)
{
    rasterize(painter, rect, color, 1.0f, [&](float ux, float uy) {
        // `ux, uy` are centre-relative; the segment is in absolute design space.
        return distance_from_segment(ux + center_x, uy + center_y, ax, ay, bx, by) <= width / 2.0f;
    });
}

static void paint_fan(Gfx::Painter& painter, Gfx::IntRect const& rect, Gfx::Color color, float opacity, bool const active_arcs[3], float dot_opacity)
{
    // The Breeze bands are one design unit wide; at small sizes that is less than
    // a device pixel and anti-aliases to a grey smear, so widen them to at least
    // one pixel.
    float const min_thickness = max(1.0f, design_size / static_cast<float>(max(rect.width(), rect.height())));

    for (int i = 0; i < 3; ++i) {
        float const outer = arc_outer_radii[i];
        float const element_opacity = active_arcs[i] ? 1.0f : inactive_opacity;
        rasterize(painter, rect, color, opacity * element_opacity, [&](float ux, float uy) {
            float const angle = atan2f(ux, -uy);
            if (fabsf(angle) > fan_half_angle)
                return false;
            float const d = hypotf(ux, uy);
            return d <= outer && d >= outer - min_thickness;
        });
    }

    float const dot_inner = max(0.0f, dot_outer_radius - min_thickness);
    rasterize(painter, rect, color, opacity * dot_opacity, [&](float ux, float uy) {
        float const d = hypotf(ux, uy);
        return d <= dot_outer_radius && d >= dot_inner;
    });
}

void paint(Gfx::Painter& painter, Gfx::IntRect const& rect, State state, int level, Gfx::Color color)
{
    if (rect.is_empty())
        return;

    bool active_arcs[3] = { false, false, false };
    float dot_opacity = inactive_opacity;
    float fan_opacity = 1.0f;

    switch (state) {
    case State::Connected:
        active_arcs[0] = level >= 100;
        active_arcs[1] = level >= 80;
        active_arcs[2] = level >= 60;
        dot_opacity = level >= 40 ? 1.0f : inactive_opacity;
        break;
    case State::Disconnected:
    case State::Connecting:
        fan_opacity = disconnected_opacity;
        break;
    case State::Off:
    case State::Unavailable:
        break;
    }

    paint_fan(painter, rect, color, fan_opacity, active_arcs, dot_opacity);

    if (state == State::Disconnected) {
        // The Breeze "network-wireless-disconnected" red X, bottom-right.
        rasterize_segment(painter, rect, 15.5f, 14.4f, 19.5f, 18.4f, 1.5f, breeze_negative);
        rasterize_segment(painter, rect, 19.5f, 14.4f, 15.5f, 18.4f, 1.5f, breeze_negative);
    } else if (state == State::Off || state == State::Unavailable) {
        // The Breeze "network-wireless-off" red slash across the whole box.
        rasterize_segment(painter, rect, 3.0f, 19.0f, 19.0f, 3.0f, 1.5f, breeze_negative);
    } else if (state == State::Connecting) {
        // The Breeze "network-wireless-acquiring" yellow spinner, top-right.
        float const spinner_x = 18.4f - center_x;
        float const spinner_y = 14.4f - center_y;
        float const spinner_radius = 1.7f;
        rasterize(painter, rect, breeze_neutral, 1.0f, [&](float ux, float uy) {
            float const dx = ux - spinner_x;
            float const dy = uy - spinner_y;
            float const d = hypotf(dx, dy);
            if (d < spinner_radius - 0.7f || d > spinner_radius)
                return false;
            // A gap in the ring, so it reads as a circular arrow.
            float const angle = atan2f(dy, dx);
            return angle < 2.2f || angle > 3.0f;
        });
        // The arrowhead at the open end.
        rasterize(painter, rect, breeze_neutral, 1.0f, [&](float ux, float uy) {
            float const dx = ux - spinner_x;
            float const dy = uy - spinner_y;
            return hypotf(dx, dy) <= 0.85f && atan2f(dy, dx) >= 2.6f && atan2f(dy, dx) <= 3.3f;
        });
    }
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> make_bitmap(Gfx::IntSize size, State state, int level, Gfx::Color color)
{
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, size));
    bitmap->fill(Gfx::Color::from_argb(0));
    Gfx::Painter painter { *bitmap };
    paint(painter, bitmap->rect(), state, level, color);
    return bitmap;
}

}
