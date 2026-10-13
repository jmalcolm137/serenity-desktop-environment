/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibGUI/Widget.h>

#include "NetworkManagerClient.h"

namespace GUI {
class Action;
class Menu;
class Window;
}

class WifiPopup;

// The Taskbar applet: a KDE-style Wi-Fi glyph that reflects the connection
// state, with a tooltip and a click-to-open networks popup. It sits in the
// applet area to the left of the battery applet.
class WifiWidget final : public GUI::Widget {
    C_OBJECT(WifiWidget)

public:
    virtual ~WifiWidget() override;

    explicit WifiWidget(NonnullRefPtr<NetworkManagerClient>);

private:
    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void context_menu_event(GUI::ContextMenuEvent&) override;
    virtual void timer_event(Core::TimerEvent&) override;
    virtual void applet_area_rect_change_event(GUI::AppletAreaRectChangeEvent&) override;

    void update_state();
    void update_tooltip();
    void set_popup_visible(bool);
    void reposition_popup();

    NonnullRefPtr<NetworkManagerClient> m_client;
    RefPtr<GUI::Window> m_popup;
    RefPtr<WifiPopup> m_popup_content;
    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Action> m_enable_action;
};
