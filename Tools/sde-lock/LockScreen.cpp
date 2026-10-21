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

ErrorOr<RefPtr<Gfx::Bitmap>> load_face(int diameter)
{
    auto* user = getpwuid(getuid());
    if (!user || !user->pw_dir)
        return RefPtr<Gfx::Bitmap> {};

    auto path = ByteString::formatted("{}/.face", user->pw_dir);
    if (access(path.characters(), R_OK) != 0)
        return RefPtr<Gfx::Bitmap> {};

    auto face = TRY(Gfx::Bitmap::load_from_file(path));
    auto scaled = TRY(face->scaled_to_size({ diameter, diameter }));

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

ErrorOr<RefPtr<Gfx::Bitmap>> make_initial_avatar(ByteString const& username, int diameter, Gfx::Font const* font)
{
    auto avatar = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { diameter, diameter }));
    memset(avatar->scanline_u8(0), 0, avatar->data_size());

    Gfx::Painter painter(*avatar);
    painter.fill_rect_with_rounded_corners(avatar->rect(), Gfx::Color(52, 84, 132), diameter / 2);

    if (font && !username.is_empty()) {
        auto initial = username.substring(0, 1).to_uppercase();
        painter.draw_text(avatar->rect().shrunken(0, 0, 2, 0), initial.view(), *font, Gfx::TextAlignment::Center, text_primary());
    }
    return RefPtr<Gfx::Bitmap> { move(avatar) };
}

}

LockRenderer::LockRenderer() = default;
LockRenderer::~LockRenderer() = default;

RefPtr<Gfx::Font> LockRenderer::font(ByteString const& family, float size, unsigned weight) const
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

ErrorOr<void> LockRenderer::initialize(ByteString const& resource_root, ByteString const& username)
{
    if (!resource_root.is_empty())
        Gfx::FontDatabase::the().load_all_fonts_from_uri(ByteString::formatted("file://{}/fonts", resource_root));

    m_clock_font = font("SerenitySans"sv, 56, 400);
    m_date_font = font("SerenitySans"sv, 15, 400);
    m_name_font = font("SerenitySans"sv, 17, 600);
    m_field_font = font("SerenitySans"sv, 15, 400);
    m_button_font = font("SerenitySans"sv, 15, 600);
    m_message_font = font("SerenitySans"sv, 13, 400);

    static constexpr int avatar_diameter = 84;
    m_avatar = TRY(load_face(avatar_diameter));
    if (!m_avatar)
        m_avatar = TRY(make_initial_avatar(username, avatar_diameter, m_clock_font.ptr()));

    return {};
}

Layout LockRenderer::layout_for(Gfx::IntSize size) const
{
    int const panel_width = min(460, max(size.width() - 32, 240));
    int const panel_height = min(410, max(size.height() - 32, 320));
    Gfx::IntRect panel {
        (size.width() - panel_width) / 2,
        (size.height() - panel_height) / 2,
        panel_width,
        panel_height,
    };

    int const center_x = panel.x() + panel.width() / 2;
    int const field_width = min(320, panel.width() - 40);

    Layout layout;
    layout.panel = panel;

    int y = panel.y() + 24;
    layout.clock = { panel.x() + 16, y, panel.width() - 32, 60 };
    y += 68;
    layout.date = { panel.x() + 16, y, panel.width() - 32, 20 };
    y += 28;
    layout.avatar = { center_x - 42, y, 84, 84 };
    y += 96;
    layout.name = { panel.x() + 16, y, panel.width() - 32, 24 };
    y += 32;
    layout.field = { center_x - field_width / 2, y, field_width, 44 };
    y += 54;
    layout.button = { center_x - field_width / 2, y, field_width, 40 };
    y += 48;
    layout.message = { panel.x() + 16, y, panel.width() - 32, 20 };
    return layout;
}

void LockRenderer::render(Gfx::Bitmap& target, UiState const& state) const
{
    Gfx::Painter painter(target);
    auto const bounds = target.rect();

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

    auto const layout = layout_for(bounds.size());

    // Panel, with a soft drop shadow and a one-pixel rounded border.
    auto const shadow = layout.panel.translated(0, 6);
    painter.fill_rect_with_rounded_corners(shadow, Gfx::Color(0, 0, 0, 96), 16);
    painter.fill_rect_with_rounded_corners(layout.panel, panel_border(), 14);
    painter.fill_rect_with_rounded_corners(layout.panel.shrunken(1, 1), panel_background(), 13);

    if (m_clock_font)
        painter.draw_text(layout.clock, state.clock.view(), *m_clock_font, Gfx::TextAlignment::Center, text_primary());
    if (m_date_font)
        painter.draw_text(layout.date, state.date.view(), *m_date_font, Gfx::TextAlignment::Center, text_secondary());

    if (m_avatar)
        painter.draw_scaled_bitmap(layout.avatar, *m_avatar, m_avatar->rect(), 1.0f, Gfx::ScalingMode::NearestNeighbor);

    if (m_name_font)
        painter.draw_text(layout.name, state.username.view(), *m_name_font, Gfx::TextAlignment::Center, text_primary());

    // Password field.
    painter.fill_rect_with_rounded_corners(layout.field, Gfx::Color(10, 12, 16, 224), 9);
    auto const field_border = state.failed ? failure() : Gfx::Color(255, 255, 255, 46);
    painter.draw_rect_with_thickness(layout.field, field_border, 1);

    auto const field_center = layout.field.center();
    if (state.password_length == 0) {
        if (m_field_font)
            painter.draw_text(layout.field, state.authenticating ? "Checking..."sv : "Password"sv, *m_field_font, Gfx::TextAlignment::Center, text_muted());
    } else {
        constexpr int dot_spacing = 13;
        constexpr int dot_size = 6;
        int const total = static_cast<int>(state.password_length - 1) * dot_spacing;
        int const start = field_center.x() - total / 2 - dot_size / 2;
        for (size_t i = 0; i < state.password_length; ++i) {
            Gfx::IntRect dot { start + static_cast<int>(i) * dot_spacing, field_center.y() - dot_size / 2, dot_size, dot_size };
            painter.fill_rect_with_rounded_corners(dot, text_primary(), dot_size / 2);
        }
    }

    if (state.caret_visible && !state.authenticating) {
        constexpr int dot_spacing = 13;
        constexpr int dot_size = 6;
        int caret_x = field_center.x();
        if (state.password_length > 0) {
            int const total = static_cast<int>(state.password_length - 1) * dot_spacing;
            caret_x = field_center.x() + total / 2 + dot_size / 2 + 6;
        }
        painter.fill_rect({ caret_x, layout.field.y() + 12, 2, layout.field.height() - 24 }, text_primary());
    }

    // Unlock button.
    painter.fill_rect_with_rounded_corners(layout.button, state.button_hovered ? accent_hovered() : accent(), 9);
    if (m_button_font)
        painter.draw_text(layout.button, state.authenticating ? "Authenticating..."sv : "Unlock"sv, *m_button_font, Gfx::TextAlignment::Center, Gfx::Color(248, 250, 253));

    // Message line: PAM's, or the Caps Lock hint when there is nothing to report.
    if (!state.message.is_empty() && m_message_font) {
        painter.draw_text(layout.message, state.message.view(), *m_message_font, Gfx::TextAlignment::Center, state.failed ? failure() : text_secondary());
    } else if (state.caps_lock && !state.authenticating && m_message_font) {
        painter.draw_text(layout.message, "Caps Lock is on"sv, *m_message_font, Gfx::TextAlignment::Center, warning());
    }
}

RefPtr<Gfx::Bitmap> LockRenderer::load_wallpaper(ByteString const& path)
{
    if (path.is_empty())
        return {};

    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto data = contents.release_value();

    if (data.size() < sizeof(WallpaperHeader))
        return {};
    WallpaperHeader header {};
    memcpy(&header, data.data(), sizeof(header));
    if (memcmp(header.magic, "PWDS", 4) != 0 || header.version != 1)
        return {};
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384)
        return {};
    if (data.size() < sizeof(header) + static_cast<size_t>(header.height) * header.stride)
        return {};

    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { static_cast<int>(header.width), static_cast<int>(header.height) });
    if (bitmap.is_error())
        return {};

    auto result = bitmap.release_value();
    for (u32 y = 0; y < header.height; ++y)
        memcpy(result->scanline_u8(static_cast<int>(y)), data.data() + sizeof(header) + static_cast<size_t>(y) * header.stride, min(static_cast<size_t>(header.width) * 4, static_cast<size_t>(header.stride)));
    return result;
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

}
