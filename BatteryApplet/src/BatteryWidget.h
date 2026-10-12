/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Widget.h>

#include "Battery.h"

// The applet's detail popup: one row per battery with the charge level, state
// and time remaining. It is custom-painted so the row layout can follow the
// number of batteries without rebuilding a widget tree on every refresh.
class BatteryDetailsWidget final : public GUI::Frame {
    C_OBJECT(BatteryDetailsWidget);

public:
    virtual ~BatteryDetailsWidget() override = default;

    void set_batteries(Vector<Battery::Status> batteries);
    Gfx::IntSize preferred_size() const;

private:
    BatteryDetailsWidget();

    virtual void paint_event(GUI::PaintEvent&) override;

    Vector<Battery::Status> m_batteries;
};

// The compact (tray) representation, modeled on KDE's battery applet: a battery
// icon whose fill tracks the charge level, a charging bolt while plugged in, an
// optional percentage label, a tooltip, and a click-to-open detail popup.
class BatteryWidget final : public GUI::Widget {
    C_OBJECT(BatteryWidget);

public:
    virtual ~BatteryWidget() override;

    bool show_percentage() const { return m_show_percentage; }
    void set_show_percentage(bool);

private:
    explicit BatteryWidget(StringView power_supply_root = "/sys/class/power_supply"sv);

    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void context_menu_event(GUI::ContextMenuEvent&) override;
    virtual void timer_event(Core::TimerEvent&) override;
    virtual void applet_area_rect_change_event(GUI::AppletAreaRectChangeEvent&) override;

    void refresh();
    void update_tooltip();
    void set_popup_visible(bool);
    void reposition_popup();

    ByteString m_power_supply_root;
    Vector<Battery::Status> m_batteries;
    Battery::Summary m_summary;
    bool m_show_percentage { true };
    RefPtr<GUI::Window> m_popup;
    RefPtr<BatteryDetailsWidget> m_details;
    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Action> m_show_percentage_action;
};
