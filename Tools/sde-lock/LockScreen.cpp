/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LockScreen.h"

#include <AK/Assertions.h>
#include <AK/FlyString.h>
#include <AK/StdLibExtras.h>
#include <LibCore/File.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Orientation.h>
#include <LibGfx/Painter.h>
#include <LibGfx/ScalingMode.h>

#include <pwd.h>
#include <string.h>
#include <unistd.h>

namespace SDELock {

namespace {

// The session's wallpaper cache, written by the compositor: "PWDS", a version, the
// width, height and stride, then BGRA pixels (WindowServer/src/Wallpaper.cpp).
struct WallpaperHeader {
    char magic[4];
    u32 version;
    u32 width;
    u32 height;
    u32 stride;
};
static_assert(sizeof(WallpaperHeader) == 20);

Gfx::Color panel_background() { return Gfx::Color(22, 24, 29, 232); }
Gfx::Color panel_border() { return Gfx::Color(255, 255, 255, 34); }
Gfx::Color text_primary() { return Gfx::Color(238, 241, 246); }
Gfx::Color text_secondary() { return Gfx::Color(166, 173, 184); }
Gfx::Color text_muted() { return Gfx::Color(120, 126, 136); }
Gfx::Color accent() { return Gfx::Color(76, 132, 214); }
Gfx::Color accent_hovered() { return Gfx::Color(96, 152, 232); }
Gfx::Color failure() { return Gfx::Color(232, 112, 112); }
Gfx::Color warning() { return Gfx::Color(232, 184, 92); }

Gfx::IntRect scale_rect(Gfx::IntRect rect, int scale)
{
    return { rect.x() * scale, rect.y() * scale, rect.width() * scale, rect.height() * scale };
}

RefPtr<Gfx::Bitmap> try_load_face_from(ByteString const& path, int diameter)
{
    if (path.is_empty() || access(path.characters(), R_OK) != 0)
        return {};

    auto face_or_error = Gfx::Bitmap::load_from_file(path);
    if (face_or_error.is_error())
        return {};
    auto scaled_or_error = face_or_error.value()->scaled_to_size({ diameter, diameter });
    if (scaled_or_error.is_error())
        return {};
    auto scaled = scaled_or_error.release_value();

    // XFCE draws the avatar in a circle; mask the corners rather than drawing a
    // square photograph over the panel.
    float const radius = diameter / 2.0f;
    for (int y = 0; y < diameter; ++y) {
        auto* row = reinterpret_cast<u32*>(scaled->scanline_u8(y));
        for (int x = 0; x < diameter; ++x) {
            float const dx = x + 0.5f - radius;
            float const dy = y + 0.5f - radius;
            if (dx * dx + dy * dy > radius * radius)
                row[x] = 0;
        }
    }
    return RefPtr<Gfx::Bitmap> { move(scaled) };
}

RefPtr<Gfx::Bitmap> load_user_avatar(ByteString const& username, int diameter)
{
    if (auto* user = getpwuid(getuid()); user && user->pw_dir) {
        if (auto bmp = try_load_face_from(ByteString::formatted("{}/.face", user->pw_dir), diameter))
            return bmp;
        if (auto bmp = try_load_face_from(ByteString::formatted("{}/.face.icon", user->pw_dir), diameter))
            return bmp;
    }
    if (!username.is_empty()) {
        if (auto bmp = try_load_face_from(ByteString::formatted("/var/lib/AccountsService/icons/{}", username), diameter))
            return bmp;
    }
    return {};
}

ErrorOr<RefPtr<Gfx::Bitmap>> make_initial_avatar(ByteString const& label, int diameter, Gfx::Font const* font)
{
    auto avatar = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { diameter, diameter }));
    memset(avatar->scanline_u8(0), 0, avatar->data_size());

    Gfx::Painter painter(*avatar);
    painter.fill_rect_with_rounded_corners(avatar->rect(), Gfx::Color(52, 84, 132), diameter / 2);

    if (font && !label.is_empty()) {
        auto initial = label.substring(0, 1).to_uppercase();
        painter.draw_text(avatar->rect().shrunken(0, 0, 2, 0), initial.view(), *font, Gfx::TextAlignment::Center, text_primary());
    }
    return RefPtr<Gfx::Bitmap> { move(avatar) };
}

}

ByteString display_name_from_gecos(ByteString const& username, StringView gecos)
{
    if (gecos.is_empty())
        return username;
    auto comma = gecos.find(',');
    auto full_name = (comma.has_value() ? gecos.substring_view(0, *comma) : gecos).trim_whitespace();
    if (full_name.is_empty())
        return username;
    return full_name.to_byte_string();
}

LockRenderer::LockRenderer() = default;
LockRenderer::~LockRenderer() = default;

RefPtr<Gfx::Font> LockRenderer::load_font(ByteString const& family, float size, unsigned weight) const
{
    auto& database = Gfx::FontDatabase::the();
    auto make_fly_string = [](StringView name) {
        return FlyString { String::from_utf8(name).release_value_but_fixme_should_propagate_errors() };
    };
    if (auto font = database.get(make_fly_string(family), size, weight, 0, 0, Gfx::Font::AllowInexactSizeMatch::Yes))
        return font;
    // Katica is the Serenity desktop font; a bitmap face is better than no text.
    return database.get(make_fly_string("Katica"sv), 10, weight, 0, 0, Gfx::Font::AllowInexactSizeMatch::Yes);
}

LockRenderer::FontSet const& LockRenderer::fonts_for_scale(int scale) const
{
    int const clamped = max(1, scale);
    if (auto it = m_fonts_by_scale.find(clamped); it != m_fonts_by_scale.end())
        return it->value;

    float const s = static_cast<float>(clamped);
    FontSet set;
    set.clock = load_font("SerenitySans"sv, 56.0f * s, 400);
    set.date = load_font("SerenitySans"sv, 15.0f * s, 400);
    set.name = load_font("SerenitySans"sv, 17.0f * s, 600);
    set.field = load_font("SerenitySans"sv, 15.0f * s, 400);
    set.button = load_font("SerenitySans"sv, 15.0f * s, 600);
    set.message = load_font("SerenitySans"sv, 13.0f * s, 400);
    set.badge = load_font("SerenitySans"sv, 11.0f * s, 600);
    m_fonts_by_scale.set(clamped, move(set));
    return m_fonts_by_scale.find(clamped)->value;
}

ErrorOr<void> LockRenderer::initialize(ByteString const& resource_root, ByteString const& username, ByteString const& display_name)
{
    if (!resource_root.is_empty())
        Gfx::FontDatabase::the().load_all_fonts_from_uri(ByteString::formatted("file://{}/fonts", resource_root));

    auto const& base_fonts = fonts_for_scale(1);

    // Render avatar at 2x (168px) so it stays crisp on both 1x and 2x displays.
    static constexpr int avatar_source_diameter = 168;
    m_avatar = load_user_avatar(username, avatar_source_diameter);
    if (!m_avatar) {
        auto const& hi_fonts = fonts_for_scale(2);
        auto const& label = display_name.is_empty() ? username : display_name;
        m_avatar = TRY(make_initial_avatar(label, avatar_source_diameter, hi_fonts.clock ? hi_fonts.clock.ptr() : base_fonts.clock.ptr()));
    }

    return {};
}

Layout LockRenderer::layout_for(Gfx::IntSize logical_size, int scale) const
{
    int const s = max(1, scale);
    int const panel_width = min(460, max(logical_size.width() - 32, 240));
    int const panel_height = min(426, max(logical_size.height() - 32, 320));
    Gfx::IntRect panel {
        (logical_size.width() - panel_width) / 2,
        (logical_size.height() - panel_height) / 2,
        panel_width,
        panel_height,
    };

    int const center_x = panel.x() + panel.width() / 2;
    int const field_width = min(320, panel.width() - 40);

    Layout layout;
    layout.panel = scale_rect(panel, s);
    layout.shadow = scale_rect(panel.translated(0, 6), s);

    int y = panel.y() + 24;
    layout.clock = scale_rect({ panel.x() + 16, y, panel.width() - 32, 60 }, s);
    y += 68;
    layout.date = scale_rect({ panel.x() + 16, y, panel.width() - 32, 20 }, s);
    y += 28;
    layout.avatar = scale_rect({ center_x - 42, y, 84, 84 }, s);
    y += 96;
    layout.name = scale_rect({ panel.x() + 16, y, panel.width() - 32, 24 }, s);
    y += 32;
    layout.field = scale_rect({ center_x - field_width / 2, y, field_width, 44 }, s);
    y += 54;
    layout.button = scale_rect({ center_x - field_width / 2, y, field_width, 40 }, s);
    y += 48;
    layout.message = scale_rect({ panel.x() + 16, y, panel.width() - 32, 20 }, s);
    layout.layout_badge = scale_rect({ panel.right() - 108, panel.bottom() - 26, 94, 18 }, s);
    return layout;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> LockRenderer::render_background(Gfx::IntSize pixel_size) const
{
    auto bg = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, pixel_size));
    Gfx::Painter painter(*bg);
    auto const bounds = bg->rect();

    if (m_wallpaper && m_wallpaper->width() > 0 && m_wallpaper->height() > 0) {
        // Cover the output without distorting the image.
        float const output_aspect = bounds.width() / static_cast<float>(bounds.height());
        float const image_aspect = m_wallpaper->width() / static_cast<float>(m_wallpaper->height());
        Gfx::IntRect source;
        if (image_aspect > output_aspect) {
            int const crop_width = static_cast<int>(m_wallpaper->height() * output_aspect);
            source = { (m_wallpaper->width() - crop_width) / 2, 0, crop_width, m_wallpaper->height() };
        } else {
            int const crop_height = static_cast<int>(m_wallpaper->width() / output_aspect);
            source = { 0, (m_wallpaper->height() - crop_height) / 2, m_wallpaper->width(), crop_height };
        }
        painter.draw_scaled_bitmap(bounds, *m_wallpaper, source, 1.0f, Gfx::ScalingMode::BilinearBlend);
        painter.fill_rect(bounds, Gfx::Color(0, 0, 0, 118));
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, bounds, Gfx::Color(0, 0, 0, 40), Gfx::Color(0, 0, 0, 168));
    } else {
        painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, bounds, Gfx::Color(30, 38, 52), Gfx::Color(8, 10, 14));
    }

    return bg;
}

void LockRenderer::render(Gfx::Bitmap& target, Gfx::Bitmap const* cached_bg, UiState const& state, Gfx::IntSize logical_size, int scale, bool show_panel, Optional<Gfx::IntRect> dirty_rect) const
{
    int const s = max(1, scale);
    Gfx::Painter painter(target);
    auto const bounds = target.rect();

    Gfx::IntRect const clip = dirty_rect.has_value() ? dirty_rect->intersected(bounds) : bounds;
    if (clip.is_empty())
        return;
    painter.add_clip_rect(clip);

    if (cached_bg && cached_bg->size() == target.size()) {
        painter.blit(clip.location(), *cached_bg, clip);
    } else {
        auto fallback = render_background(target.size());
        if (!fallback.is_error())
            painter.blit(clip.location(), *fallback.value(), clip);
    }

    auto const& fonts = fonts_for_scale(s);

    if (!show_panel) {
        // Secondary outputs display only the dimmed wallpaper + centered clock/date.
        int const clock_h = 64 * s;
        int const date_h = 22 * s;
        int const total_h = clock_h + 8 * s + date_h;
        int const top = (bounds.height() - total_h) / 2;
        Gfx::IntRect clock_rect { 0, top, bounds.width(), clock_h };
        Gfx::IntRect date_rect { 0, top + clock_h + 8 * s, bounds.width(), date_h };
        if (fonts.clock)
            painter.draw_text(clock_rect, state.clock.view(), *fonts.clock, Gfx::TextAlignment::Center, text_primary());
        if (fonts.date)
            painter.draw_text(date_rect, state.date.view(), *fonts.date, Gfx::TextAlignment::Center, text_secondary());
        return;
    }

    auto const layout = layout_for(logical_size, s);

    // Panel, with a soft drop shadow and a one-pixel rounded border.
    painter.fill_rect_with_rounded_corners(layout.shadow, Gfx::Color(0, 0, 0, 96), 16 * s);
    painter.fill_rect_with_rounded_corners(layout.panel, panel_border(), 14 * s);
    painter.fill_rect_with_rounded_corners(layout.panel.shrunken(s, s), panel_background(), 13 * s);

    if (fonts.clock)
        painter.draw_text(layout.clock, state.clock.view(), *fonts.clock, Gfx::TextAlignment::Center, text_primary());
    if (fonts.date)
        painter.draw_text(layout.date, state.date.view(), *fonts.date, Gfx::TextAlignment::Center, text_secondary());

    if (m_avatar)
        painter.draw_scaled_bitmap(layout.avatar, *m_avatar, m_avatar->rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);

    if (fonts.name)
        painter.draw_text(layout.name, state.display_name.view(), *fonts.name, Gfx::TextAlignment::Center, text_primary());

    // Password field.
    painter.fill_rect_with_rounded_corners(layout.field, Gfx::Color(10, 12, 16, 224), 9 * s);
    auto const field_border = state.failed ? failure() : Gfx::Color(255, 255, 255, 46);
    painter.draw_rect_with_thickness(layout.field, field_border, s);

    auto const field_inner = layout.field.shrunken(14 * s, 4 * s);
    auto const field_center = layout.field.center();
    int const dot_spacing = 13 * s;
    int const dot_size = 6 * s;

    // Cap visible dots so long passwords never overflow the input box.
    size_t const max_visible_dots = static_cast<size_t>(max(1, (field_inner.width() - 16 * s) / dot_spacing));
    size_t const visible_dots = min(state.password_length, max_visible_dots);

    if (state.password_length == 0) {
        if (fonts.field)
            painter.draw_text(layout.field, state.authenticating ? "Checking..."sv : "Password"sv, *fonts.field, Gfx::TextAlignment::Center, text_muted());
    } else {
        Gfx::PainterStateSaver saver(painter);
        painter.add_clip_rect(field_inner);

        int const total = static_cast<int>(visible_dots - 1) * dot_spacing;
        int const start = field_center.x() - total / 2 - dot_size / 2;
        for (size_t i = 0; i < visible_dots; ++i) {
            Gfx::IntRect dot { start + static_cast<int>(i) * dot_spacing, field_center.y() - dot_size / 2, dot_size, dot_size };
            painter.fill_rect_with_rounded_corners(dot, text_primary(), dot_size / 2);
        }
    }

    if (state.caret_visible && !state.authenticating) {
        int caret_x = field_center.x();
        if (visible_dots > 0) {
            int const total = static_cast<int>(visible_dots - 1) * dot_spacing;
            caret_x = min(field_inner.right() - 2 * s, field_center.x() + total / 2 + dot_size / 2 + 6 * s);
        }
        painter.fill_rect({ caret_x, layout.field.y() + 12 * s, 2 * s, layout.field.height() - 24 * s }, text_primary());
    }

    // Unlock button.
    painter.fill_rect_with_rounded_corners(layout.button, state.button_hovered ? accent_hovered() : accent(), 9 * s);
    if (fonts.button)
        painter.draw_text(layout.button, state.authenticating ? "Authenticating..."sv : "Unlock"sv, *fonts.button, Gfx::TextAlignment::Center, Gfx::Color(248, 250, 253));

    // Message line: PAM's, or the Caps Lock hint when there is nothing to report.
    if (!state.message.is_empty() && fonts.message) {
        painter.draw_text(layout.message, state.message.view(), *fonts.message, Gfx::TextAlignment::Center, state.failed ? failure() : text_secondary());
    } else if (state.caps_lock && !state.authenticating && fonts.message) {
        painter.draw_text(layout.message, "Caps Lock is on"sv, *fonts.message, Gfx::TextAlignment::Center, warning());
    }

    // Keyboard layout badge when multiple XKB layouts are configured.
    if (!state.keyboard_layout.is_empty() && fonts.badge) {
        painter.draw_text(layout.layout_badge, state.keyboard_layout.view(), *fonts.badge, Gfx::TextAlignment::CenterRight, text_muted());
    }
}

RefPtr<Gfx::Bitmap> LockRenderer::load_wallpaper(ByteString const& path)
{
    if (path.is_empty() || access(path.characters(), R_OK) != 0)
        return {};

    // First try the compositor's "PWDS" wallpaper.raw cache format.
    if (auto file = Core::File::open(path, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            auto data = contents.release_value();
            if (data.size() >= sizeof(WallpaperHeader)) {
                WallpaperHeader header {};
                memcpy(&header, data.data(), sizeof(header));
                if (memcmp(header.magic, "PWDS", 4) == 0 && header.version == 1
                    && header.width > 0 && header.height > 0 && header.width <= 16384 && header.height <= 16384
                    && data.size() >= sizeof(header) + static_cast<size_t>(header.height) * header.stride) {
                    if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { static_cast<int>(header.width), static_cast<int>(header.height) }); !bitmap.is_error()) {
                        auto result = bitmap.release_value();
                        for (u32 y = 0; y < header.height; ++y)
                            memcpy(result->scanline_u8(static_cast<int>(y)), data.data() + sizeof(header) + static_cast<size_t>(y) * header.stride, min(static_cast<size_t>(header.width) * 4, static_cast<size_t>(header.stride)));
                        return result;
                    }
                }
            }
        }
    }

    // Fallback: standard image file (PNG, JPEG, BMP, ...) via LibGfx.
    if (auto loaded = Gfx::Bitmap::load_from_file(path); !loaded.is_error())
        return loaded.release_value();
    return {};
}

ByteString LockRenderer::default_wallpaper_path()
{
    if (auto const* state = getenv("SDE_STATE_DIR"); state && *state) {
        auto path = ByteString::formatted("{}/wallpaper.raw", state);
        if (access(path.characters(), R_OK) == 0)
            return path;
    }

    auto* user = getpwuid(getuid());
    ByteString config;
    if (auto const* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        config = ByteString(xdg);
    else if (user && user->pw_dir)
        config = ByteString::formatted("{}/.config", user->pw_dir);
    else
        return {};

    auto path = ByteString::formatted("{}/sde/wallpaper.raw", config);
    if (access(path.characters(), R_OK) == 0)
        return path;
    return {};
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> LockRenderer::make_cursor_bitmap(int scale)
{
    int const s = max(1, scale);
    int const size = 16 * s;
    auto bmp = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { size, size }));
    memset(bmp->scanline_u8(0), 0, bmp->data_size());

    // Crisp 16x16 arrow cursor bitmap (1 = dark border, 2 = white fill).
    static constexpr char const* arrow[16] = {
        "1...............",
        "11..............",
        "121.............",
        "1221............",
        "12221...........",
        "122221..........",
        "1222221.........",
        "12222221........",
        "122222221.......",
        "1222222221......",
        "12222211111.....",
        "1221221.........",
        "121.1221........",
        "11..1221........",
        "1....11.........",
        "................",
    };

    Gfx::Painter painter(*bmp);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            char c = arrow[y][x];
            if (c == '1')
                painter.fill_rect({ x * s, y * s, s, s }, Gfx::Color(16, 18, 22));
            else if (c == '2')
                painter.fill_rect({ x * s, y * s, s, s }, Gfx::Color(246, 248, 252));
        }
    }
    return bmp;
}

}
