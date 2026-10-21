/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WifiPopup.h"
#include "WifiIcon.h"

#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Label.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/ScrollableContainerWidget.h>
#include <LibGUI/SeparatorWidget.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>

#include <stdlib.h>

namespace {

// The popup's grid. Everything lines up on these.
constexpr int popup_width = 320;
constexpr int margin = 8;
constexpr int spacing = 6;
constexpr int header_height = 36;
constexpr int separator_height = 2;
constexpr int control_height = 24;
constexpr int list_frame = 2;
constexpr int max_visible_rows = 8;
constexpr int empty_list_rows = 3;
constexpr int search_threshold = 8;
constexpr int hidden_form_height = 3 * control_height + 2 * spacing + 18;
constexpr int hidden_label_width = 64;

// Inside a row.
constexpr int row_padding = 8;
constexpr int icon_size = 16;
constexpr int text_x = row_padding + icon_size + 8;
constexpr int lock_width = 7;
constexpr int action_button_width = 92;

// A connection attempt that NetworkManager has not even started on by now has
// failed (it refused, or the network is gone).
constexpr int attempt_start_timeout_ms = 15000;

String network_title(WifiNetwork const& network)
{
    if (auto decoded = String::from_utf8(network.ssid.view()); !decoded.is_error())
        return decoded.release_value();
    return MUST(String::formatted("<{} bytes>", network.ssid.length()));
}

String ssid_title(ByteString const& ssid)
{
    WifiNetwork network;
    network.ssid = ssid;
    return network_title(network);
}

Gfx::Color muted(Gfx::Color color)
{
    // A solid muted colour: the Classic theme's disabled text colour is a dither
    // pattern, which reduces small text to speckle.
    return color.with_alpha(150);
}

Gfx::Color error_color()
{
    return Gfx::Color(178, 34, 34);
}

// A 7x9 padlock, drawn on the pixel grid so it stays crisp at 1x.
void paint_lock(Gfx::Painter& painter, Gfx::IntPoint origin, Gfx::Color color)
{
    auto px = [&](int x, int y, int w, int h) { painter.fill_rect({ origin.x() + x, origin.y() + y, w, h }, color); };
    px(2, 0, 3, 1); // shackle
    px(1, 1, 1, 3);
    px(5, 1, 1, 3);
    px(0, 4, 7, 5); // body
}

// The keyhole is punched out of the body in the row's background colour.
void paint_keyhole(Gfx::Painter& painter, Gfx::IntPoint origin, Gfx::Color background)
{
    painter.fill_rect({ origin.x() + 3, origin.y() + 6, 1, 2 }, background);
}

StringView band_name(int frequency)
{
    if (frequency >= 5925)
        return "6 GHz"sv;
    if (frequency >= 4900)
        return "5 GHz"sv;
    if (frequency > 0)
        return "2.4 GHz"sv;
    return {};
}

ByteString find_settings_tool()
{
    for (auto path : { "/usr/bin/nm-connection-editor"sv, "/usr/local/bin/nm-connection-editor"sv }) {
        if (!Core::System::access(path, X_OK).is_error())
            return path;
    }
    return {};
}

// The title, the status line under it, and the radio's switch.
class PopupHeader final : public GUI::Widget {
    C_OBJECT(PopupHeader);

public:
    void set_status(String status, bool is_error)
    {
        if (m_status == status && m_is_error == is_error)
            return;
        m_status = move(status);
        m_is_error = is_error;
        update();
    }

    GUI::CheckBox& switch_box() { return *m_switch; }

private:
    PopupHeader()
    {
        set_fixed_height(header_height);
        m_switch = add<GUI::CheckBox>("Enabled"_string);
        m_switch->set_tooltip("Turn Wi-Fi on or off"_string);
        // Opening the popup must not land the keyboard on the switch.
        m_switch->set_focus_policy(GUI::FocusPolicy::TabFocus);
    }

    virtual void resize_event(GUI::ResizeEvent& event) override
    {
        GUI::Widget::resize_event(event);
        int switch_width = static_cast<int>(font().width("Enabled"sv)) + 24;
        m_switch->set_relative_rect({ width() - switch_width, 0, switch_width, 18 });
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Widget::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        auto& bold = Gfx::FontDatabase::default_font().bold_variant();
        painter.draw_text(Gfx::IntRect { 0, 0, width() / 2, 18 }, "Wi-Fi"sv, bold, Gfx::TextAlignment::CenterLeft, palette().window_text());
        auto color = m_is_error ? error_color() : muted(palette().window_text());
        painter.draw_text(Gfx::IntRect { 0, 18, width(), 18 }, m_status, font(), Gfx::TextAlignment::CenterLeft, color, Gfx::TextElision::Right);
    }

    RefPtr<GUI::CheckBox> m_switch;
    String m_status;
    bool m_is_error { false };
};

}

// --- WifiRow ---------------------------------------------------------------

WifiRow::WifiRow()
{
    set_fixed_height(line_height);
}

void WifiRow::set_network(WifiNetwork const& network, WifiRowStatus status)
{
    m_network = network;
    m_status = status;
    if (m_action_button)
        m_action_button->set_text(m_network.connected ? "Disconnect"_string : "Connect"_string);
    update();
}

void WifiRow::set_open(bool open, bool wants_password)
{
    if (m_open == open && m_wants_password == wants_password) {
        if (m_action_button)
            m_action_button->set_text(m_network.connected ? "Disconnect"_string : "Connect"_string);
        return;
    }
    m_open = open;
    m_wants_password = wants_password;
    set_fixed_height(height_for_state());

    if (m_open && !m_action_button) {
        m_action_button = add<GUI::Button>();
        m_action_button->set_focus_policy(GUI::FocusPolicy::TabFocus);
        m_action_button->on_click = [this](auto) {
            if (on_action)
                on_action();
        };
    }
    if (m_action_button) {
        m_action_button->set_text(m_network.connected ? "Disconnect"_string : "Connect"_string);
        m_action_button->set_visible(m_open);
    }

    if (m_open && m_wants_password && !m_password_box) {
        // A PasswordBox masks what is typed; a TextBox marked secret only stops
        // it being copied.
        auto& password_box = add<GUI::PasswordBox>();
        password_box.set_show_reveal_button(true);
        m_password_box = password_box;
        m_password_box->set_placeholder("Password"_string);
        m_password_box->on_return_pressed = [this] {
            if (on_action)
                on_action();
        };
        m_password_box->on_escape_pressed = [this] {
            if (on_cancel)
                on_cancel();
        };
        m_password_box->on_change = [this] {
            if (on_password_edited)
                on_password_edited(m_password_box->text());
        };
    }
    if (m_password_box)
        m_password_box->set_visible(m_open && m_wants_password);

    layout_actions();
    update();
}

ByteString WifiRow::password() const
{
    if (!m_password_box || !m_password_box->is_visible())
        return {};
    return m_password_box->text();
}

void WifiRow::set_password(ByteString const& password)
{
    if (m_password_box && m_password_box->text() != password)
        m_password_box->set_text(password);
}

void WifiRow::focus_password()
{
    if (m_password_box && m_password_box->is_visible())
        m_password_box->set_focus(true);
}

void WifiRow::layout_actions()
{
    int const y = line_height + 1;
    int const button_x = width() - row_padding - action_button_width;
    if (m_action_button)
        m_action_button->set_relative_rect({ button_x, y, action_button_width, control_height });
    if (m_password_box)
        m_password_box->set_relative_rect({ text_x, y, max(40, button_x - spacing - text_x), control_height });
}

void WifiRow::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    auto background = palette().base();
    if (m_open)
        background = background.mixed_with(palette().selection(), 0.16f);
    else if (m_hovered)
        background = background.mixed_with(palette().selection(), 0.08f);
    painter.fill_rect(rect(), background);

    auto text_color = palette().base_text();
    auto quiet = muted(text_color);
    auto& font = Gfx::FontDatabase::default_font();
    auto& bold = font.bold_variant();

    // Line one: signal, name, status, lock. The status and lock columns are fixed,
    // so they line up down the list whether or not a row uses them.
    Gfx::IntRect icon_rect { row_padding, (line_height - icon_size) / 2, icon_size, icon_size };
    WifiIcon::paint(painter, icon_rect, WifiIcon::State::Connected, WifiIcon::level_for_strength(m_network.strength), text_color);

    int const lock_x = width() - row_padding - lock_width;
    int const status_right = lock_x - 8;
    // As wide as the widest status, so every status ends in the same column.
    int const status_width = static_cast<int>(font.width("Connecting…"sv)) + 2;
    int const status_x = status_right - status_width;

    StringView status;
    auto status_color = quiet;
    switch (m_status) {
    case WifiRowStatus::Connected:
        status = "Connected"sv;
        status_color = text_color;
        break;
    case WifiRowStatus::Connecting:
        status = "Connecting…"sv;
        break;
    case WifiRowStatus::Failed:
        status = "Failed"sv;
        status_color = error_color();
        break;
    case WifiRowStatus::Saved:
        status = "Saved"sv;
        break;
    case WifiRowStatus::None:
        break;
    }

    int const name_right = status.is_empty() ? status_right : status_x - 8;
    painter.draw_text(Gfx::IntRect { text_x, 0, name_right - text_x, line_height }, network_title(m_network),
        m_network.connected ? bold : font, Gfx::TextAlignment::CenterLeft, text_color, Gfx::TextElision::Right);
    if (!status.is_empty())
        painter.draw_text(Gfx::IntRect { status_x, 0, status_width, line_height }, status, font, Gfx::TextAlignment::CenterRight, status_color);

    if (m_network.secured) {
        Gfx::IntPoint lock_origin { lock_x, (line_height - 9) / 2 };
        paint_lock(painter, lock_origin, quiet);
        paint_keyhole(painter, lock_origin, background);
    }

    // Line two (open rows): details, unless the passphrase field takes the room.
    if (m_open && !m_wants_password) {
        StringBuilder details;
        details.appendff("Signal {}%", m_network.strength);
        if (auto band = band_name(m_network.frequency); !band.is_empty())
            details.appendff(" · {}", band);
        int const button_x = width() - row_padding - action_button_width;
        painter.draw_text(Gfx::IntRect { text_x, line_height + 1, button_x - spacing - text_x, control_height }, details.string_view(),
            font, Gfx::TextAlignment::CenterLeft, quiet, Gfx::TextElision::Right);
    }
}

void WifiRow::resize_event(GUI::ResizeEvent& event)
{
    GUI::Widget::resize_event(event);
    layout_actions();
}

void WifiRow::mousedown_event(GUI::MouseEvent& event)
{
    // Only the row's first line toggles it; its controls take their own clicks.
    if (event.button() == GUI::MouseButton::Primary && event.y() < line_height && on_toggle)
        on_toggle();
}

void WifiRow::mousemove_event(GUI::MouseEvent&)
{
    if (!m_hovered) {
        m_hovered = true;
        update();
    }
}

void WifiRow::leave_event(Core::Event& event)
{
    if (m_hovered) {
        m_hovered = false;
        update();
    }
    GUI::Widget::leave_event(event);
}

void WifiRow::keydown_event(GUI::KeyEvent& event)
{
    if (event.key() == Key_Escape && m_open && on_cancel) {
        on_cancel();
        event.accept();
        return;
    }
    GUI::Widget::keydown_event(event);
}

// --- WifiPopup -------------------------------------------------------------

WifiPopup::WifiPopup() = default;

void WifiPopup::set_client(NonnullRefPtr<NetworkManagerClient> client)
{
    m_client = move(client);
    rebuild();
}

Gfx::IntSize WifiPopup::preferred_size() const
{
    int height = 2 * frame_thickness() + 2 * margin;
    int visible_children = 0;
    for (auto const& child : children()) {
        if (!is<GUI::Widget>(*child))
            continue;
        auto const* widget = static_cast<GUI::Widget const*>(child.ptr());
        if (widget->is_visible()) {
            height += widget->min_height().as_int();
            ++visible_children;
        }
    }
    height += max(0, visible_children - 1) * spacing;
    return { popup_width, height };
}

void WifiPopup::build_chrome()
{
    set_frame_style(Gfx::FrameStyle::Window);
    set_fill_with_background_color(true);
    set_layout<GUI::VerticalBoxLayout>(GUI::Margins { margin }, spacing);

    auto& header = add<PopupHeader>();
    m_header = header;
    m_enabled_box = header.switch_box();
    m_enabled_box->on_checked = [this](bool checked) {
        if (m_client)
            m_client->set_wireless_enabled(checked);
    };

    auto& separator = add<GUI::HorizontalSeparator>();
    separator.set_fixed_height(separator_height);

    m_search = add<GUI::TextBox>();
    m_search->set_placeholder("Search networks"_string);
    m_search->set_fixed_height(control_height);
    m_search->set_visible(false);
    m_search->on_change = [this] {
        m_filter = m_search->text();
        schedule_rebuild();
    };

    m_scroll = add<GUI::ScrollableContainerWidget>();
    m_scroll->set_frame_style(Gfx::FrameStyle::SunkenContainer);
    m_scroll->set_should_hide_unnecessary_scrollbars(true);
    m_list = GUI::Widget::construct();
    m_list->set_fill_with_background_color(true);
    m_list->set_background_role(Gfx::ColorRole::Base);
    m_list->set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, 0);
    m_scroll->set_widget(m_list.ptr());

    m_placeholder = GUI::Label::construct();
    m_placeholder->set_text_alignment(Gfx::TextAlignment::Center);
    m_placeholder->set_foreground_role(Gfx::ColorRole::BaseText);
    m_placeholder->set_fixed_height(empty_list_rows * WifiRow::line_height);

    // The hidden-network form: labels in one column, fields in the next.
    m_hidden_form = add<GUI::Widget>();
    m_hidden_form->set_fixed_height(hidden_form_height);
    m_hidden_form->set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, spacing);
    auto& form_title = m_hidden_form->add<GUI::Label>("Connect to a hidden network"_string);
    form_title.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    form_title.set_font(Gfx::FontDatabase::default_font().bold_variant());
    form_title.set_fixed_height(18 - spacing);
    auto form_line = [&](String label, RefPtr<GUI::TextBox>& box, bool is_password) {
        auto& line = m_hidden_form->add<GUI::Widget>();
        line.set_fixed_height(control_height);
        line.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, spacing);
        auto& caption = line.add<GUI::Label>(move(label));
        caption.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        caption.set_fixed_width(hidden_label_width);
        if (is_password) {
            auto& password_box = line.add<GUI::PasswordBox>();
            password_box.set_show_reveal_button(true);
            box = password_box;
        } else {
            box = line.add<GUI::TextBox>();
        }
    };
    form_line("Name:"_string, m_hidden_ssid, false);
    form_line("Password:"_string, m_hidden_password, true);
    m_hidden_password->set_placeholder("None for an open network"_string);
    m_hidden_password->on_return_pressed = [this] { connect_hidden(); };
    m_hidden_ssid->on_return_pressed = [this] { m_hidden_password->set_focus(true); };
    m_hidden_ssid->on_escape_pressed = [this] { set_hidden_form_visible(false); };
    m_hidden_password->on_escape_pressed = [this] { set_hidden_form_visible(false); };
    auto& form_buttons = m_hidden_form->add<GUI::Widget>();
    form_buttons.set_fixed_height(control_height);
    form_buttons.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, spacing);
    form_buttons.add_spacer();
    auto& cancel = form_buttons.add<GUI::Button>("Cancel"_string);
    cancel.set_fixed_width(80);
    cancel.on_click = [this](auto) { set_hidden_form_visible(false); };
    auto& connect = form_buttons.add<GUI::Button>("Connect"_string);
    connect.set_fixed_width(80);
    connect.on_click = [this](auto) { connect_hidden(); };
    m_hidden_form->set_visible(false);

    m_footer = add<GUI::Widget>();
    m_footer->set_fixed_height(control_height);
    m_footer->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins {}, spacing);
    m_hidden_button = m_footer->add<GUI::Button>("Hidden Network…"_string);
    m_hidden_button->set_fixed_width(120);
    m_hidden_button->set_focus_policy(GUI::FocusPolicy::TabFocus);
    m_hidden_button->set_tooltip("Connect to a network that does not broadcast its name"_string);
    m_hidden_button->on_click = [this](auto) { set_hidden_form_visible(true); };
    m_footer->add_spacer();
    m_settings_path = find_settings_tool();
    m_settings_button = m_footer->add<GUI::Button>("Network Settings…"_string);
    m_settings_button->set_fixed_width(130);
    m_settings_button->set_focus_policy(GUI::FocusPolicy::TabFocus);
    m_settings_button->set_tooltip("Edit saved connections"_string);
    // Without NetworkManager's connection editor there is nothing to open.
    m_settings_button->set_visible(!m_settings_path.is_empty());
    m_settings_button->on_click = [this](auto) {
        GUI::Process::spawn_or_show_error(window(), m_settings_path, ReadonlySpan<char const*> {});
    };
}

WifiRowStatus WifiPopup::status_for(WifiNetwork const& network) const
{
    if (network.connected)
        return WifiRowStatus::Connected;
    if (!m_attempt_ssid.is_empty() && network.ssid == m_attempt_ssid)
        return WifiRowStatus::Connecting;
    if (!m_failed_ssid.is_empty() && network.ssid == m_failed_ssid)
        return WifiRowStatus::Failed;
    if (network.saved)
        return WifiRowStatus::Saved;
    return WifiRowStatus::None;
}

bool WifiPopup::wants_password(WifiNetwork const& network) const
{
    if (network.connected)
        return false;
    // NetworkManager's secret agent asked, or a secured network we have no
    // profile (and so no passphrase) for.
    return network.ssid == m_password_ssid || (network.secured && !network.saved);
}

void WifiPopup::update_attempt()
{
    if (m_attempt_ssid.is_empty() || !m_client)
        return;
    auto const& state = m_client->state();
    if (state.active_ssid == m_attempt_ssid) {
        m_attempt_ssid = {};
        m_failed_ssid = {};
        m_attempt_timer = nullptr;
        return;
    }
    if (state.connecting) {
        m_attempt_seen_connecting = true;
        // NetworkManager is working on it; activation can take a while (DHCP).
        m_attempt_timer = nullptr;
        return;
    }
    // It was coming up and stopped without connecting, or NetworkManager went.
    if (m_attempt_seen_connecting || !state.available) {
        m_failed_ssid = m_attempt_ssid;
        m_attempt_ssid = {};
        m_attempt_timer = nullptr;
    }
}

void WifiPopup::start_attempt(ByteString const& ssid)
{
    m_attempt_ssid = ssid;
    m_attempt_seen_connecting = false;
    m_failed_ssid = {};
    m_attempt_timer = Core::Timer::create_single_shot(attempt_start_timeout_ms, [this] {
        if (m_attempt_ssid.is_empty() || m_attempt_seen_connecting)
            return;
        m_failed_ssid = m_attempt_ssid;
        m_attempt_ssid = {};
        schedule_rebuild();
    });
    m_attempt_timer->start();
}

void WifiPopup::update_header()
{
    auto const& state = m_client->state();
    bool radio_usable = state.available && state.networking_enabled && state.wireless_hardware_enabled;
    m_enabled_box->set_checked(radio_usable && state.wireless_enabled, GUI::AllowCallback::No);
    m_enabled_box->set_enabled(radio_usable);

    auto& header = static_cast<PopupHeader&>(*m_header);
    if (!state.available)
        header.set_status("Unavailable"_string, false);
    else if (!state.networking_enabled)
        header.set_status("Networking is disabled"_string, false);
    else if (!state.wireless_hardware_enabled)
        header.set_status("Turned off by the hardware switch"_string, false);
    else if (!state.wireless_enabled)
        header.set_status("Off"_string, false);
    else if (!m_attempt_ssid.is_empty())
        header.set_status(MUST(String::formatted("Connecting to {}…", ssid_title(m_attempt_ssid))), false);
    else if (!m_failed_ssid.is_empty())
        header.set_status(MUST(String::formatted("Could not connect to {}", ssid_title(m_failed_ssid))), true);
    else if (!state.active_ssid.is_empty())
        header.set_status(MUST(String::formatted("Connected to {}", ssid_title(state.active_ssid))), false);
    else if (state.connecting)
        header.set_status("Connecting…"_string, false);
    else
        header.set_status("Not connected"_string, false);
}

void WifiPopup::rebuild()
{
    m_rebuild_scheduled = false;
    if (!m_client)
        return;
    if (!m_header)
        build_chrome();

    update_attempt();
    update_header();

    auto const& state = m_client->state();
    auto const& networks = m_client->networks();
    bool radio_on = state.available && state.wireless_enabled && state.wireless_hardware_enabled && state.networking_enabled;

    m_search->set_visible(radio_on && (networks.size() > search_threshold || !m_filter.is_empty()));
    m_hidden_button->set_enabled(radio_on);
    m_settings_button->set_enabled(state.available);
    if (!radio_on && m_hidden_form->is_visible())
        set_hidden_form_visible(false);

    Vector<WifiNetwork const&> shown;
    if (radio_on) {
        for (auto const& network : networks) {
            if (m_filter.is_empty() || network_title(network).contains(m_filter, CaseSensitivity::CaseInsensitive))
                shown.append(network);
        }
    }

    // The open row must still exist (it may have gone out of range).
    bool open_still_listed = false;
    for (auto const& network : shown)
        open_still_listed |= network.ssid == m_open_ssid;
    if (!open_still_listed && !m_open_ssid.is_empty() && m_open_ssid != m_password_ssid)
        m_open_ssid = {};

    // Rows are reused when the list keeps its order, so a refresh in the middle
    // of typing a passphrase does not take the field (or its focus) away.
    bool same_order = m_rows.size() == shown.size();
    for (size_t i = 0; same_order && i < shown.size(); ++i)
        same_order = m_rows[i]->ssid() == shown[i].ssid;

    if (!same_order) {
        m_rows.clear();
        m_list->remove_all_children();
        for (auto const& network : shown) {
            auto& row = m_list->add<WifiRow>();
            auto ssid = network.ssid;
            row.on_toggle = [this, ssid] { toggle_row(ssid); };
            row.on_action = [this, ssid] { row_action(ssid); };
            row.on_cancel = [this] {
                if (!m_password_ssid.is_empty())
                    cancel_password();
                m_open_ssid = {};
                m_password_draft = {};
                schedule_rebuild();
            };
            row.on_password_edited = [this](ByteString const& text) { m_password_draft = text; };
            m_rows.append(row);
        }
    }
    if (shown.is_empty() && !m_placeholder->parent())
        m_list->add_child(*m_placeholder);

    int list_content = 0;
    int open_extra = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        auto& row = *m_rows[i];
        auto const& network = shown[i];
        row.set_network(network, status_for(network));
        bool open = network.ssid == m_open_ssid;
        bool was_open = row.is_open();
        row.set_open(open, wants_password(network));
        if (open) {
            row.set_password(m_password_draft);
            if (!was_open)
                row.focus_password();
            open_extra = WifiRow::actions_height;
        }
        list_content += WifiRow::line_height;
    }

    if (shown.is_empty()) {
        if (!state.available)
            m_placeholder->set_text("NetworkManager is not running."_string);
        else if (!state.networking_enabled || !state.wireless_hardware_enabled)
            m_placeholder->set_text("Wi-Fi is unavailable."_string);
        else if (!state.wireless_enabled)
            m_placeholder->set_text("Turn on Wi-Fi to see networks."_string);
        else if (!m_filter.is_empty())
            m_placeholder->set_text("No networks match."_string);
        else
            m_placeholder->set_text("Looking for networks…"_string);
        list_content = empty_list_rows * WifiRow::line_height;
    }

    int visible = min(list_content, max_visible_rows * WifiRow::line_height) + open_extra;
    m_scroll->set_fixed_height(visible + 2 * list_frame);
    m_list->set_min_height(list_content + open_extra);

    if (on_preferred_size_changed)
        on_preferred_size_changed();
    if (getenv("WIFI_APPLET_LOG_LAYOUT"))
        deferred_invoke([this] { log_layout(); });
}

void WifiPopup::log_layout()
{
    // For the tests, which have to click on rows: where each one is on screen.
    if (!window() || !window()->is_visible())
        return;
    auto origin = window()->rect().location();
    for (auto& row : m_rows) {
        auto rect = row->window_relative_rect().translated(origin);
        dbgln("WifiApplet: layout row '{}' {}x{}+{},{}", row->ssid(), rect.width(), rect.height(), rect.x(), rect.y());
    }
}

void WifiPopup::refresh()
{
    rebuild();
}

void WifiPopup::schedule_rebuild()
{
    // Rebuilding may destroy the list widgets, so it must never run inside one of
    // their event handlers (e.g. a row's click). Defer to the next loop turn.
    if (m_rebuild_scheduled)
        return;
    m_rebuild_scheduled = true;
    WeakPtr<WifiPopup> weak = this;
    Core::deferred_invoke([weak] {
        if (auto* popup = weak.ptr())
            popup->rebuild();
    });
}

void WifiPopup::toggle_row(ByteString const& ssid)
{
    if (m_open_ssid == ssid) {
        if (m_password_ssid == ssid)
            cancel_password();
        m_open_ssid = {};
    } else {
        // Opening another row abandons a prompt NetworkManager is waiting on.
        if (!m_password_ssid.is_empty() && m_password_ssid != ssid)
            cancel_password();
        m_open_ssid = ssid;
    }
    m_password_draft = {};
    schedule_rebuild();
}

void WifiPopup::row_action(ByteString const& ssid)
{
    if (!m_client)
        return;
    Optional<WifiNetwork> network;
    for (auto const& candidate : m_client->networks()) {
        if (candidate.ssid == ssid)
            network = candidate;
    }
    if (!network.has_value())
        return;

    dbgln("WifiApplet: activate ssid='{}' (secured={}, saved={}, connected={})", network->ssid, network->secured, network->saved, network->connected);

    if (network->connected) {
        m_client->disconnect(*network);
        m_open_ssid = {};
        schedule_rebuild();
        return;
    }

    if (wants_password(*network)) {
        if (m_password_draft.is_empty()) {
            for (auto& row : m_rows) {
                if (row->ssid() == ssid)
                    row->focus_password();
            }
            return;
        }
        if (m_password_ssid == ssid) {
            submit_password();
        } else {
            m_client->connect_to(*network, m_password_draft);
        }
    } else {
        m_client->connect_to(*network, {});
    }
    start_attempt(ssid);
    m_open_ssid = {};
    m_password_draft = {};
    schedule_rebuild();
}

void WifiPopup::prompt_for_password(ByteString const& ssid, Function<void(Optional<ByteString>)> callback)
{
    // A prompt that is replaced still owes its caller an answer: NetworkManager
    // waits for one.
    if (m_password_callback.has_value()) {
        auto previous = move(*m_password_callback);
        m_password_callback = {};
        previous({});
    }
    m_password_callback = move(callback);
    m_password_ssid = ssid;
    m_open_ssid = ssid;
    m_password_draft = {};
    schedule_rebuild();
}

void WifiPopup::submit_password()
{
    auto password = m_password_draft;
    m_password_ssid = {};
    m_password_draft = {};
    m_open_ssid = {};
    if (m_password_callback.has_value()) {
        auto callback = move(*m_password_callback);
        m_password_callback = {};
        callback(password.is_empty() ? Optional<ByteString> {} : Optional<ByteString> { password });
    }
    schedule_rebuild();
}

void WifiPopup::cancel_password()
{
    if (m_attempt_ssid == m_password_ssid)
        m_attempt_ssid = {};
    if (m_open_ssid == m_password_ssid)
        m_open_ssid = {};
    m_password_ssid = {};
    m_password_draft = {};
    if (m_password_callback.has_value()) {
        auto callback = move(*m_password_callback);
        m_password_callback = {};
        callback({});
    }
    schedule_rebuild();
}

void WifiPopup::set_hidden_form_visible(bool visible)
{
    if (!m_hidden_form)
        return;
    m_hidden_form->set_visible(visible);
    m_footer->set_visible(!visible);
    if (visible) {
        m_hidden_ssid->set_text({});
        m_hidden_password->set_text({});
        m_hidden_ssid->set_focus(true);
    }
    if (on_preferred_size_changed)
        on_preferred_size_changed();
}

void WifiPopup::connect_hidden()
{
    auto ssid = m_hidden_ssid->text();
    if (ssid.is_empty()) {
        m_hidden_ssid->set_focus(true);
        return;
    }
    WifiNetwork network;
    network.ssid = ssid;
    network.secured = !m_hidden_password->text().is_empty();
    network.device_path = m_client->state().wifi_device_path;
    m_client->connect_to(network, network.secured ? Optional<ByteString> { m_hidden_password->text() } : Optional<ByteString> {});
    start_attempt(ssid);
    set_hidden_form_visible(false);
    schedule_rebuild();
}

void WifiPopup::show_hidden_form()
{
    if (!m_header)
        rebuild();
    if (m_hidden_button->is_enabled())
        set_hidden_form_visible(true);
}

void WifiPopup::restore_focus()
{
    if (m_hidden_form && m_hidden_form->is_visible()) {
        if (!m_hidden_ssid->is_focused() && !m_hidden_password->is_focused())
            m_hidden_ssid->set_focus(true);
        return;
    }
    for (auto& row : m_rows) {
        if (row->is_open())
            row->focus_password();
    }
}

void WifiPopup::popup_shown()
{
    m_failed_ssid = {};
    rebuild();
}

void WifiPopup::popup_hidden()
{
    // Closing the popup is an answer too.
    if (!m_password_ssid.is_empty())
        cancel_password();
    m_open_ssid = {};
    m_password_draft = {};
    if (m_hidden_form && m_hidden_form->is_visible())
        set_hidden_form_visible(false);
}

void WifiPopup::keydown_event(GUI::KeyEvent& event)
{
    if (event.key() == Key_Escape) {
        if (!m_open_ssid.is_empty()) {
            if (!m_password_ssid.is_empty())
                cancel_password();
            m_open_ssid = {};
            schedule_rebuild();
        } else if (m_hidden_form && m_hidden_form->is_visible()) {
            set_hidden_form_visible(false);
        } else if (on_close_requested) {
            on_close_requested();
        }
        event.accept();
        return;
    }
    GUI::Frame::keydown_event(event);
}
