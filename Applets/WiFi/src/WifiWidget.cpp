/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WifiWidget.h"
#include "WifiIcon.h"
#include "WifiPopup.h"

#include <LibCore/System.h>
#include <LibGUI/Action.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <LibGfx/Palette.h>

WifiWidget::~WifiWidget() = default;

WifiWidget::WifiWidget(NonnullRefPtr<NetworkManagerClient> client)
    : m_client(move(client))
{
    m_client->on_changed = [this] { update_state(); };
    m_client->on_password_required = [this](ByteString const& ssid, Function<void(Optional<ByteString>)> callback) {
        set_popup_visible(true);
        if (m_popup_content)
            m_popup_content->prompt_for_password(ssid, move(callback));
    };
    m_client->on_password_request_cancelled = [this] {
        if (m_popup_content)
            m_popup_content->cancel_password();
    };

    update_tooltip();
    start_timer(5000);
}

void WifiWidget::update_state()
{
    update_tooltip();
    if (m_popup_content && m_popup && m_popup->is_visible())
        m_popup_content->refresh();
    update();
}

void WifiWidget::update_tooltip()
{
    // The popup says all of this and more; a tooltip on top of it only hides it.
    if (m_popup && m_popup->is_visible()) {
        set_tooltip({});
        return;
    }
    auto const& state = m_client->state();
    if (!state.available) {
        set_tooltip("NetworkManager is not available"_string);
        return;
    }
    if (!state.networking_enabled || !state.wireless_hardware_enabled) {
        set_tooltip("Networking is disabled"_string);
        return;
    }
    if (!state.wireless_enabled) {
        set_tooltip("Wi-Fi is off"_string);
        return;
    }
    if (!state.active_ssid.is_empty()) {
        String name = "Wireless"_string;
        if (auto decoded = String::from_utf8(state.active_ssid.view()); !decoded.is_error())
            name = decoded.release_value();
        set_tooltip(MUST(String::formatted("Networks\n{} ({}%)", name, state.active_strength)));
        return;
    }
    set_tooltip(state.connecting ? "Networks\nConnecting…"_string : "Networks\nDisconnected"_string);
}

void WifiWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Widget::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.clear_rect(event.rect(), Gfx::Color::from_argb(0));

    auto const& state = m_client->state();
    auto icon_state = WifiIcon::State::Disconnected;
    int level = 0;

    if (!state.available || !state.networking_enabled || !state.wireless_hardware_enabled) {
        icon_state = WifiIcon::State::Unavailable;
    } else if (state.connecting) {
        icon_state = WifiIcon::State::Connecting;
    } else if (!state.active_ssid.is_empty()) {
        icon_state = WifiIcon::State::Connected;
        level = WifiIcon::level_for_strength(state.active_strength);
    } else {
        icon_state = WifiIcon::State::Disconnected;
    }

    WifiIcon::paint(painter, rect(), icon_state, level, palette().window_text());
}

void WifiWidget::mousedown_event(GUI::MouseEvent& event)
{
    if (event.button() == GUI::MouseButton::Primary)
        set_popup_visible(!m_popup || !m_popup->is_visible());
}

void WifiWidget::timer_event(Core::TimerEvent&)
{
    m_client->refresh();
    update_tooltip();
}

void WifiWidget::applet_area_rect_change_event(GUI::AppletAreaRectChangeEvent&)
{
    reposition_popup();
}

void WifiWidget::set_popup_visible(bool visible)
{
    if (visible && !m_popup) {
        // The popup is a child of this widget (like the battery applet's), so it
        // is destroyed with the applet.
        m_popup = add<GUI::Window>();
        m_popup->set_title("Wi-Fi");
        m_popup->set_window_type(GUI::WindowType::Popup);
        m_popup->set_resizable(false);
        m_popup_content = m_popup->set_main_widget<WifiPopup>();
        m_popup_content->on_preferred_size_changed = [this] {
            if (m_popup && m_popup->is_visible())
                reposition_popup();
        };
        m_popup_content->on_close_requested = [this] { set_popup_visible(false); };
        m_popup_content->set_client(m_client);
    }

    if (!m_popup)
        return;

    if (visible) {
        m_popup_content->popup_shown();
        reposition_popup();
        m_popup->show();
        m_client->request_scan();
    } else {
        m_popup_content->popup_hidden();
        m_popup->hide();
    }
    update_tooltip();
}

void WifiWidget::reposition_popup()
{
    if (!m_popup || !m_popup_content)
        return;

    auto size = m_popup_content->preferred_size();
    auto applet_rect = window()->rect();
    constexpr int margin = 4;

    int x = applet_rect.right() - size.width();
    int y = applet_rect.y() - size.height() - margin;
    if (x < 0)
        x = 0;
    // Not enough room above the applet area: drop below it instead.
    if (y < 0)
        y = applet_rect.bottom() + margin;

    // A popup layer surface is fixed-size, so a size change (an expanded row, or
    // the hidden-network form) has to be re-applied by hiding and showing it.
    if (m_popup->is_visible() && (m_popup->width() != size.width() || m_popup->height() != size.height())) {
        m_popup->hide();
        m_popup->set_rect(x, y, size.width(), size.height());
        m_popup->show();
        m_popup_content->restore_focus();
        return;
    }
    m_popup->set_rect(x, y, size.width(), size.height());
}

void WifiWidget::context_menu_event(GUI::ContextMenuEvent& event)
{
    event.accept();

    if (!m_context_menu) {
        m_context_menu = GUI::Menu::construct();

        auto enable_action = GUI::Action::create_checkable("Enable Wi-Fi", [this](GUI::Action& action) {
            m_client->set_wireless_enabled(action.is_checked());
        });
        m_enable_action = enable_action;
        m_context_menu->add_action(*enable_action);

        m_context_menu->add_separator();

        m_context_menu->add_action(GUI::Action::create("Connect to Hidden Network…", [this](GUI::Action&) {
            set_popup_visible(true);
            if (m_popup_content)
                m_popup_content->show_hidden_form();
        }));

        // NetworkManager's connection editor, if it is installed: without it the
        // entry would do nothing, so it is not offered.
        for (auto path : { "/usr/bin/nm-connection-editor"sv, "/usr/local/bin/nm-connection-editor"sv }) {
            if (Core::System::access(path, X_OK).is_error())
                continue;
            m_context_menu->add_action(GUI::Action::create("Configure Network Connections…", [this, path = ByteString { path }](GUI::Action&) {
                GUI::Process::spawn_or_show_error(window(), path, ReadonlySpan<char const*> {});
            }));
            break;
        }
    }

    auto const& state = m_client->state();
    bool radio_usable = state.available && state.networking_enabled && state.wireless_hardware_enabled;
    m_enable_action->set_checked(radio_usable && state.wireless_enabled);
    m_enable_action->set_enabled(radio_usable);
    m_context_menu->popup(event.screen_position());
}
