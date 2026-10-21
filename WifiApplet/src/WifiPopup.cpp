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
#include <LibGUI/Label.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/ScrollableContainerWidget.h>
#include <LibGUI/TextBox.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>

namespace {

constexpr int popup_width = 340;
constexpr int row_height = 44;
constexpr int expanded_row_height = 74;
constexpr int toolbar_height = 34;
constexpr int max_list_height = 340;
constexpr int icon_size = 22;

String network_title(WifiNetwork const& network)
{
    if (auto decoded = String::from_utf8(network.ssid.view()); !decoded.is_error())
        return decoded.release_value();
    return MUST(String::formatted("<{} bytes>", network.ssid.length()));
}

// A small padlock, drawn rather than loaded so the applet needs no assets.
void paint_lock(Gfx::Painter& painter, Gfx::IntRect const& rect, Gfx::Color color)
{
    int const body_height = rect.height() * 2 / 3;
    Gfx::IntRect body { rect.x(), rect.bottom() - body_height + 1, rect.width(), body_height };
    painter.fill_rect(body, color);
    // Shackle: an open-bottomed U above the body.
    int const top = rect.y();
    int const mid = rect.y() + (body.y() - rect.y()) / 2;
    int const left = rect.x() + 1;
    int const right = rect.right() - 1;
    painter.draw_line({ left, body.y() }, { left, mid }, color);
    painter.draw_line({ right, body.y() }, { right, mid }, color);
    painter.draw_line({ left, mid }, { right, mid }, color);
}

}

// --- WifiRow ---------------------------------------------------------------

WifiRow::WifiRow() = default;

void WifiRow::set_network(WifiNetwork const& network)
{
    m_network = network;
    update();
}

void WifiRow::set_expanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;

    if (m_expanded) {
        set_min_size(Gfx::IntSize { 0, expanded_row_height });
        set_preferred_size(popup_width - 8, expanded_row_height);
        if (!m_password_box) {
            m_password_box = add<GUI::TextBox>();
            m_password_box->set_text_is_secret(true);
            m_password_box->set_placeholder("Password"_string);
            m_password_box->set_min_size(Gfx::IntSize { 150, 24 });
            m_password_box->on_return_pressed = [this] { if (on_submit_password) on_submit_password(); };
        }
        m_password_box->set_visible(true);
        // Position it explicitly now (the row may not get a resize event before
        // the next frame; resize_event refines the width when the row is laid out).
        int box_width = max(60, width() - 40 - 46);
        m_password_box->set_relative_rect(Gfx::IntRect { 40, 38, box_width, 26 });
        m_password_box->set_focus(true);
    } else {
        set_min_size(Gfx::IntSize { 0, row_height });
        set_preferred_size(popup_width - 8, row_height);
        if (m_password_box)
            m_password_box->set_visible(false);
    }
    if (parent_widget())
        parent_widget()->do_layout();
    update();
}

ByteString WifiRow::password() const
{
    if (!m_password_box)
        return {};
    return m_password_box->text();
}

void WifiRow::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    if (m_hovered)
        painter.fill_rect(rect(), palette().selection().with_alpha(45));

    constexpr int padding = 8;
    constexpr int icon_column = 40;
    Gfx::IntRect icon_rect { padding + 2, (row_height - icon_size) / 2, icon_size, icon_size };
    auto text_color = palette().window_text();
    // A solid muted colour: the Classic theme's disabled text colour is a dither
    // pattern, which reduces small subtitle text to speckle.
    auto muted_color = palette().window_text().with_alpha(150);

    bool connected = m_network.connected;
    WifiIcon::paint(painter, icon_rect, WifiIcon::State::Connected, WifiIcon::level_for_strength(m_network.strength), text_color);

    int title_y = m_expanded ? 6 : 7;
    auto& title_font = Gfx::FontDatabase::default_font();
    painter.draw_text(Gfx::IntRect { icon_column, title_y, width() - icon_column - 8, 16 }, network_title(m_network), title_font, Gfx::TextAlignment::CenterLeft, connected ? palette().selection() : text_color);

    ByteString subtitle;
    if (m_expanded)
        subtitle = "Enter the password";
    else if (connected)
        subtitle = "Connected";
    else if (m_network.saved)
        subtitle = "Saved";
    else
        subtitle = ByteString::formatted("{}%", m_network.strength);
    painter.draw_text(Gfx::IntRect { icon_column, title_y + 18, width() - icon_column - 8, 16 }, subtitle, title_font, Gfx::TextAlignment::CenterLeft, muted_color);

    if (m_network.secured) {
        Gfx::IntRect lock { width() - (connected ? 96 : 26), (row_height - 11) / 2, 8, 11 };
        paint_lock(painter, lock, muted_color);
    }

    if (m_hovered && !m_expanded) {
        auto action = connected ? "Disconnect"sv : "Connect"sv;
        auto action_color = connected ? palette().disabled_text_front() : palette().selection();
        auto text_value = connected ? "Connected"_string : ""_string;
        (void)text_value;
        painter.draw_text(Gfx::IntRect { width() - 90, (row_height - 16) / 2, 82, 16 }, action, title_font, Gfx::TextAlignment::CenterRight, action_color);
    }
}

void WifiRow::resize_event(GUI::ResizeEvent& event)
{
    GUI::Widget::resize_event(event);
    if (m_password_box) {
        constexpr int icon_column = 40;
        int box_width = max(60, width() - icon_column - 46);
        m_password_box->set_relative_rect(Gfx::IntRect { icon_column, 38, box_width, 26 });
    }
}

void WifiRow::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() != GUI::MouseButton::Primary)
        return;
    if (m_expanded) {
        // A click on the row (but not the field itself, which consumes its own
        // events) submits a non-empty passphrase.
        if (m_password_box && !m_password_box->text().is_empty() && on_submit_password)
            on_submit_password();
        return;
    }
    if (on_activate)
        on_activate(m_network);
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
    if (event.key() == Key_Escape && m_expanded) {
        if (on_cancel_password)
            on_cancel_password();
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
    int visible_rows = static_cast<int>(m_rows.size());
    int list_height = clamp(visible_rows * row_height + (m_expanded_ssid.is_empty() ? 0 : expanded_row_height - row_height), 0, max_list_height);
    if (list_height == 0)
        list_height = 80;
    int hidden = (m_hidden_form && m_hidden_form->is_visible()) ? 76 : 0;
    return { popup_width, toolbar_height + list_height + hidden + 16 };
}

void WifiPopup::update_wifi_button()
{
    if (!m_client || !m_wifi_button)
        return;
    auto const& state = m_client->state();
    bool on = state.networking_enabled && state.wireless_hardware_enabled && state.wireless_enabled;
    auto icon_state = on ? WifiIcon::State::Connected : WifiIcon::State::Off;
    if (auto icon = WifiIcon::make_bitmap({ 18, 18 }, icon_state, 100, palette().window_text()); !icon.is_error())
        m_wifi_button->set_icon(icon.release_value());
    m_wifi_button->set_checked(on);
    m_wifi_button->set_enabled(state.available && state.networking_enabled && state.wireless_hardware_enabled);
}

void WifiPopup::rebuild()
{
    m_rebuild_scheduled = false;
    if (!m_client)
        return;

    // Build the static chrome once.
    if (!m_wifi_button) {
        set_frame_style(Gfx::FrameStyle::Window);
        set_fill_with_background_color(true);
        set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 5, 5, 5, 5 }, 5);

        auto& toolbar = add<GUI::Widget>();
        toolbar.set_preferred_size(popup_width - 10, toolbar_height);
        toolbar.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 3, 3, 3, 3 }, 5);

        m_wifi_button = toolbar.add<GUI::Button>();
        m_wifi_button->set_button_style(Gfx::ButtonStyle::Coolbar);
        m_wifi_button->set_checkable(true);
        m_wifi_button->set_tooltip("Enable Wi-Fi"_string);
        m_wifi_button->on_click = [this](auto) {
            if (m_client)
                m_client->set_wireless_enabled(m_wifi_button->is_checked());
        };

        toolbar.add_spacer();

        m_search = toolbar.add<GUI::TextBox>();
        m_search->set_placeholder("Search…"_string);
        m_search->set_preferred_size(120, 0);
        m_search->on_change = [this] {
            m_filter = m_search->text();
            schedule_rebuild();
        };

        auto& hidden_button = toolbar.add<GUI::Button>("Hidden…"_string);
        hidden_button.set_button_style(Gfx::ButtonStyle::Coolbar);
        hidden_button.set_tooltip("Connect to a hidden network"_string);
        hidden_button.on_click = [this](auto) { set_hidden_form_visible(!m_hidden_form->is_visible()); };

        m_configure_button = toolbar.add<GUI::Button>();
        m_configure_button->set_button_style(Gfx::ButtonStyle::Coolbar);
        m_configure_button->set_text("Configure"_string);
        m_configure_button->set_tooltip("Configure network connections…"_string);
        m_configure_button->on_click = [this](auto) {
            for (auto path : { "/usr/bin/nm-connection-editor"sv, "/usr/local/bin/nm-connection-editor"sv }) {
                if (Core::System::access(path, X_OK).is_error())
                    continue;
                GUI::Process::spawn_or_show_error(window(), path, ReadonlySpan<char const*> {});
                break;
            }
        };

        m_scroll = add<GUI::ScrollableContainerWidget>();
        m_scroll->set_min_size(Gfx::IntSize { 0, 0 });
        m_scroll->set_should_hide_unnecessary_scrollbars(true);

        m_list = GUI::Widget::construct();
        m_list->set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, 2);
        // The list is the scroll content, so it must not shrink to the viewport.
        m_scroll->set_widget(m_list.ptr());

        m_placeholder = add<GUI::Label>();
        m_placeholder->set_text_alignment(Gfx::TextAlignment::Center);
        m_placeholder->set_visible(false);

        // Hidden-network form (SSID + password), toggled from the toolbar button.
        m_hidden_form = add<GUI::Widget>();
        m_hidden_form->set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, 3);
        m_hidden_form->set_min_size(Gfx::IntSize { popup_width - 10, 76 });
        m_hidden_form->set_max_size({ popup_width - 10, 76 });
        m_hidden_form->set_preferred_size(popup_width - 10, 76);
        m_hidden_ssid = m_hidden_form->add<GUI::TextBox>();
        m_hidden_ssid->set_placeholder("Network name (SSID)"_string);
        m_hidden_password = m_hidden_form->add<GUI::TextBox>();
        m_hidden_password->set_placeholder("Password (leave empty for open)"_string);
        m_hidden_password->set_text_is_secret(true);
        auto& hidden_connect = m_hidden_form->add<GUI::Button>("Connect"_string);
        hidden_connect.on_click = [this](auto) {
            auto ssid = m_hidden_ssid->text();
            if (ssid.is_empty())
                return;
            WifiNetwork network;
            network.ssid = ssid;
            network.secured = !m_hidden_password->text().is_empty();
            network.device_path = m_client->state().wifi_device_path;
            m_client->connect_to(network, m_hidden_password->text().is_empty() ? Optional<ByteString> {} : m_hidden_password->text());
        };
        m_hidden_form->set_visible(false);
    }

    update_wifi_button();

    // Rebuild the rows from the current snapshot.
    m_rows.clear();
    m_list->remove_all_children();
    auto const& networks = m_client->networks();
    int shown = 0;
    for (auto const& network : networks) {
        if (!m_filter.is_empty() && !network_title(network).contains(m_filter, CaseSensitivity::CaseInsensitive))
            continue;
        auto& row = m_list->add<WifiRow>();
        row.set_network(network);
        row.set_min_size(Gfx::IntSize { 0, row_height });
        row.set_preferred_size(popup_width - 30, row_height);
        row.on_activate = [this](WifiNetwork const& n) { activate(n); };
        row.on_submit_password = [this] { submit_password(); };
        row.on_cancel_password = [this] { cancel_password(); };
        if (!m_expanded_ssid.is_empty() && network.ssid == m_expanded_ssid)
            row.set_expanded(true);
        m_rows.append(row);
        ++shown;
    }

    bool empty = shown == 0;
    m_placeholder->set_visible(empty);
    if (empty) {
        auto const& state = m_client->state();
        if (!state.available)
            m_placeholder->set_text("NetworkManager is not available"_string);
        else if (!state.wireless_enabled || !state.wireless_hardware_enabled)
            m_placeholder->set_text("Wi-Fi is deactivated"_string);
        else if (state.connecting)
            m_placeholder->set_text("Connecting…"_string);
        else if (!m_filter.is_empty())
            m_placeholder->set_text("No matches"_string);
        else
            m_placeholder->set_text("No available connections"_string);
    }

    if (on_preferred_size_changed)
        on_preferred_size_changed();
}

void WifiPopup::refresh()
{
    rebuild();
}

void WifiPopup::schedule_rebuild()
{
    // Rebuilding destroys the list widgets, so it must never run inside one of
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

void WifiPopup::activate(WifiNetwork const& network)
{
    if (!m_client)
        return;

    dbgln("WifiApplet: activate ssid='{}' (secured={}, saved={}, connected={})", network.ssid, network.secured, network.saved, network.connected);

    if (network.connected) {
        m_client->disconnect(network);
        return;
    }

    if (network.secured) {
        // Prompt for the passphrase; the callback connects (or hands it to
        // NetworkManager's secret agent).
        auto client = m_client;
        prompt_for_password(network.ssid, [client, network](Optional<ByteString> password) {
            if (password.has_value())
                client->connect_to(network, password);
        });
        return;
    }

    m_client->connect_to(network, {});
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
    m_expanded_ssid = ssid;
    m_password_callback = move(callback);
    schedule_rebuild();
}

void WifiPopup::submit_password()
{
    auto password = ByteString {};
    for (auto& row : m_rows) {
        if (row->is_expanded()) {
            password = row->password();
            break;
        }
    }
    m_expanded_ssid = {};
    auto callback = Optional<Function<void(Optional<ByteString>)>> {};
    if (m_password_callback.has_value()) {
        callback = move(*m_password_callback);
        m_password_callback = {};
    }
    schedule_rebuild();
    if (callback.has_value())
        (*callback)(password.is_empty() ? Optional<ByteString> {} : Optional<ByteString> { password });
}

void WifiPopup::cancel_password()
{
    m_expanded_ssid = {};
    if (m_password_callback.has_value()) {
        auto callback = move(*m_password_callback);
        m_password_callback = {};
        schedule_rebuild();
        callback({});
        return;
    }
    schedule_rebuild();
}

void WifiPopup::set_hidden_form_visible(bool visible)
{
    if (m_hidden_form)
        m_hidden_form->set_visible(visible);
    if (on_preferred_size_changed)
        on_preferred_size_changed();
}

void WifiPopup::show_hidden_form()
{
    set_hidden_form_visible(true);
}
