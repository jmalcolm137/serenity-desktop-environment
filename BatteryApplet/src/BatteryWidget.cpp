/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BatteryWidget.h"

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <LibGUI/Action.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>

// The compact icon geometry: a 16x16 cell with the battery body and its
// terminal nub, leaving room for an optional percentage label to the right.
static constexpr int icon_cell_size = 16;
static constexpr int percentage_text_x = 18;

// Breeze-inspired charge colours, as KDE uses on its battery icon.
static Gfx::Color charge_color(int percentage)
{
    if (percentage <= 5)
        return Gfx::Color::from_rgb(0xda4453); // Negative red.
    if (percentage <= 20)
        return Gfx::Color::from_rgb(0xf67400); // Neutral orange.
    return Gfx::Color::from_rgb(0x27ae60);     // Positive green.
}

// A machine with no battery (a desktop) runs from mains, so the tray shows a
// plug rather than an empty battery. The same glyph is used while a battery is
// charging, where the charge level is shown by the percentage label. The two
// prongs, the plug body and the cable are drawn at the icon cell's scale, like
// the battery glyph.
static void paint_plug(Gfx::Painter& painter, Gfx::Color color)
{
    painter.fill_rect({ 4, 1, 2, 4 }, color);
    painter.fill_rect({ 10, 1, 2, 4 }, color);
    painter.fill_rect({ 3, 5, 10, 5 }, color);

    // The cable leaves the body downwards and bends to the right.
    painter.fill_rect({ 7, 10, 2, 2 }, color);
    painter.draw_line({ 8, 12 }, { 8, 14 }, color);
    painter.draw_line({ 8, 14 }, { 11, 14 }, color);
}

BatteryDetailsWidget::BatteryDetailsWidget()
{
    set_frame_style(Gfx::FrameStyle::Window);
}

void BatteryDetailsWidget::set_batteries(Vector<Battery::Status> batteries)
{
    m_batteries = move(batteries);
    update();
}

Gfx::IntSize BatteryDetailsWidget::preferred_size() const
{
    constexpr int width = 220;
    constexpr int padding = 10;
    constexpr int title_height = 18;
    constexpr int separator_gap = 8;
    constexpr int row_height = 44;

    int height = padding + title_height + separator_gap + padding;
    height += m_batteries.is_empty() ? title_height : static_cast<int>(m_batteries.size()) * row_height;
    return { width, height };
}

void BatteryDetailsWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    // The plain Classic window frame only draws its border (the default theme
    // has no corner radius), so fill the interior explicitly.
    painter.fill_rect(frame_inner_rect(), palette().window());

    constexpr int padding = 10;
    auto text_color = palette().window_text();
    auto muted_color = palette().disabled_text_front();
    auto& font = Gfx::FontDatabase::default_font();

    int y = padding;
    painter.draw_text(Gfx::IntRect { padding, y, width() - 2 * padding, 16 }, "Battery"sv, font, Gfx::TextAlignment::CenterLeft, text_color);
    y += 18;
    painter.draw_line({ padding, y }, { width() - padding - 1, y }, palette().threed_shadow1());
    y += 8;

    if (m_batteries.is_empty()) {
        painter.draw_text(Gfx::IntRect { padding, y, width() - 2 * padding, 16 }, "Running on mains power"sv, font, Gfx::TextAlignment::CenterLeft, muted_color);
        return;
    }

    for (auto const& battery : m_batteries) {
        auto title = battery.model.is_empty()
            ? battery.name
            : ByteString::formatted("{} ({})", battery.name, battery.model);
        painter.draw_text(Gfx::IntRect { padding, y, width() - 2 * padding, 16 }, title, font, Gfx::TextAlignment::CenterLeft, text_color);
        y += 16;

        Gfx::IntRect bar { padding, y, width() - 2 * padding, 8 };
        painter.fill_rect(bar, palette().base());
        int fill_width = (bar.width() * battery.percentage) / 100;
        if (fill_width > 0)
            painter.fill_rect({ bar.x(), bar.y(), fill_width, bar.height() }, charge_color(battery.percentage));
        painter.draw_rect(bar, palette().threed_shadow1());
        y += 12;

        auto detail = ByteString::formatted("{}% \xE2\x80\x94 {}", battery.percentage, Battery::format_time_remaining(battery.state, battery.seconds_remaining));
        painter.draw_text(Gfx::IntRect { padding, y, width() - 2 * padding, 16 }, detail, font, Gfx::TextAlignment::CenterLeft, text_color);
        y += 20;
    }
}

BatteryWidget::~BatteryWidget() = default;

BatteryWidget::BatteryWidget(StringView power_supply_root)
    : m_power_supply_root(power_supply_root)
{
    refresh();
    start_timer(5000);
}

void BatteryWidget::set_show_percentage(bool show_percentage)
{
    m_show_percentage = show_percentage;

    if (auto* applet_window = window()) {
        if (m_show_percentage && m_summary.has_battery) {
            auto& font = Gfx::FontDatabase::default_font();
            auto text_width = static_cast<int>(font.width("100%"sv));
            applet_window->resize(percentage_text_x + text_width + 4, icon_cell_size);
        } else {
            applet_window->resize(icon_cell_size, icon_cell_size);
        }
    }

    update();
    update_tooltip();
}

void BatteryWidget::refresh()
{
    auto batteries = Battery::read_all(m_power_supply_root);
    if (batteries.is_error()) {
        // A machine with no power-supply class at all (a plain desktop, a
        // container) runs from mains: that is the expected state, not a failure,
        // and it must not log a line every poll.
        static bool s_reported_missing = false;
        if (!s_reported_missing) {
            dbgln("BatteryApplet: no power-supply class at {}; assuming mains power", m_power_supply_root);
            s_reported_missing = true;
        }
        m_batteries.clear();
    } else {
        m_batteries = batteries.release_value();
    }

    m_summary = Battery::summarize(m_batteries);
    update_tooltip();

    // Log the compact state only when it changes, so a long-running session does
    // not flood the log with a line every poll.
    static ByteString s_last_state;
    auto state = m_summary.has_battery
        ? ByteString::formatted("{} batteries, {}%, {}", m_summary.battery_count, m_summary.percentage, Battery::state_name(m_summary.state))
        : ByteString { "no battery" };
    if (state != s_last_state) {
        s_last_state = state;
        dbgln("BatteryApplet: {}", state);
    }

    if (m_details)
        m_details->set_batteries(m_batteries);
    if (m_popup && m_popup->is_visible())
        reposition_popup();

    update();
}

void BatteryWidget::update_tooltip()
{
    if (!m_summary.has_battery) {
        set_tooltip("Running on mains power (no battery)"_string);
        return;
    }

    if (m_summary.battery_count == 1) {
        auto time = Battery::format_time_remaining(m_summary.state, m_summary.seconds_remaining);
        set_tooltip(MUST(String::formatted("Battery: {}% ({})", m_summary.percentage, time)));
        return;
    }

    StringBuilder builder;
    builder.appendff("Batteries: {}%", m_summary.percentage);
    for (auto const& battery : m_batteries)
        builder.appendff("\n{}: {}%", battery.name, battery.percentage);
    set_tooltip(MUST(builder.to_string()));
}

void BatteryWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.clear_rect(event.rect(), Gfx::Color::from_argb(0));

    auto outline_color = palette().window_text();

    // While the charger is connected (charging or already full), show a plug
    // instead of the battery glyph. The percentage label still reports the
    // charge level. A desktop with no battery gets the same plug.
    bool const on_charger = m_summary.state == Battery::State::Charging || m_summary.state == Battery::State::Full;
    if (m_summary.has_battery && !on_charger) {
        Gfx::IntRect body { 1, 4, 11, 9 };
        Gfx::IntRect terminal { body.right() + 1, 7, 2, 3 };
        painter.draw_rect(body, outline_color);
        painter.fill_rect(terminal, outline_color);

        auto inner = body.shrunken(1, 1);
        int fill_width = (inner.width() * m_summary.percentage) / 100;
        if (fill_width > 0)
            painter.fill_rect({ inner.x(), inner.y(), fill_width, inner.height() }, charge_color(m_summary.percentage));
    } else {
        paint_plug(painter, outline_color);
    }

    if (m_show_percentage && m_summary.has_battery) {
        auto text = ByteString::formatted("{}%", m_summary.percentage);
        painter.draw_text(Gfx::IntRect { percentage_text_x, 0, width() - percentage_text_x - 2, height() }, text, Gfx::FontDatabase::default_font(), Gfx::TextAlignment::CenterLeft, outline_color);
    }
}

void BatteryWidget::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() == GUI::MouseButton::Primary)
        set_popup_visible(!m_popup || !m_popup->is_visible());
}

void BatteryWidget::timer_event(Core::TimerEvent&)
{
    refresh();
}

void BatteryWidget::applet_area_rect_change_event(GUI::AppletAreaRectChangeEvent&)
{
    reposition_popup();
}

void BatteryWidget::set_popup_visible(bool visible)
{
    if (visible && !m_popup) {
        // The popup is a child of this widget (like the Audio applet's slider),
        // so it is destroyed with the applet. Do not pass window() as the
        // Window's parent: add_child() would then see a foreign parent.
        m_popup = add<GUI::Window>();
        m_popup->set_title("Battery");
        m_popup->set_window_type(GUI::WindowType::Popup);
        m_popup->set_resizable(false);
        m_details = m_popup->set_main_widget<BatteryDetailsWidget>();
    }

    if (!m_popup)
        return;

    if (visible) {
        m_details->set_batteries(m_batteries);
        reposition_popup();
        m_popup->show();
    } else {
        m_popup->hide();
    }
}

void BatteryWidget::reposition_popup()
{
    if (!m_popup)
        return;

    auto size = m_details->preferred_size();
    auto applet_rect = window()->rect();
    constexpr int margin = 4;

    int x = applet_rect.right() - size.width();
    int y = applet_rect.y() - size.height() - margin;
    if (x < 0)
        x = 0;
    // Not enough room above the applet area: drop below it instead.
    if (y < 0)
        y = applet_rect.bottom() + margin;

    m_popup->set_rect(x, y, size.width(), size.height());
}

void BatteryWidget::context_menu_event(GUI::ContextMenuEvent& event)
{
    event.accept();

    if (!m_context_menu) {
        m_context_menu = GUI::Menu::construct();
        m_show_percentage_action = GUI::Action::create_checkable("Show Percentage", [this](GUI::Action& action) {
            set_show_percentage(action.is_checked());
        });
        m_context_menu->add_action(*m_show_percentage_action);
    }

    m_show_percentage_action->set_checked(m_show_percentage);
    m_context_menu->popup(event.screen_position());
}
