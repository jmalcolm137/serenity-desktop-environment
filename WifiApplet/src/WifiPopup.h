/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Widget.h>

#include "NetworkManagerClient.h"

namespace GUI {
class Button;
class Label;
class ScrollableContainerWidget;
class TextBox;
}

// One network in the popup list, laid out like plasma-nm's ConnectionItem: the
// signal icon, the SSID, a subtitle (state or last-used), a lock for a secured
// network, and a Connect/Disconnect action. A secured, unsaved network expands
// in place to a passphrase field, as KDE does.
class WifiRow final : public GUI::Widget {
    C_OBJECT(WifiRow);

public:
    virtual ~WifiRow() override = default;

    void set_network(WifiNetwork const& network);
    void set_expanded(bool expanded);
    bool is_expanded() const { return m_expanded; }
    ByteString const& ssid() const { return m_network.ssid; }
    ByteString password() const;

    Function<void(WifiNetwork const&)> on_activate;
    Function<void()> on_submit_password;
    Function<void()> on_cancel_password;

private:
    WifiRow();

    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void resize_event(GUI::ResizeEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;
    virtual void keydown_event(GUI::KeyEvent&) override;

    WifiNetwork m_network;
    bool m_expanded { false };
    bool m_hovered { false };
    RefPtr<GUI::TextBox> m_password_box;
};

// The KDE-style networks popup: a toolbar (Wi-Fi switch, search, configure), the
// connection list, a hidden-network form, and a placeholder when there is
// nothing to show.
class WifiPopup final : public GUI::Frame {
    C_OBJECT(WifiPopup);

public:
    virtual ~WifiPopup() override = default;

    void set_client(NonnullRefPtr<NetworkManagerClient>);
    Gfx::IntSize preferred_size() const;

    // Re-read the client's snapshot and rebuild the list.
    void refresh();

    // Address a passphrase request (from an activation, or NetworkManager's
    // secret agent). The callback must be invoked exactly once.
    void prompt_for_password(ByteString const& ssid, Function<void(Optional<ByteString>)>);
    // Closes the passphrase prompt as if the user had cancelled it.
    void cancel_password();

    // Reveal the "connect to a hidden network" form.
    void show_hidden_form();

    Function<void()> on_preferred_size_changed;

private:
    WifiPopup();

    void rebuild();
    void schedule_rebuild();
    void activate(WifiNetwork const&);
    void submit_password();
    void update_wifi_button();
    void set_hidden_form_visible(bool);

    RefPtr<NetworkManagerClient> m_client;
    RefPtr<GUI::Button> m_wifi_button;
    RefPtr<GUI::TextBox> m_search;
    RefPtr<GUI::Button> m_configure_button;
    RefPtr<GUI::ScrollableContainerWidget> m_scroll;
    RefPtr<GUI::Widget> m_list;
    RefPtr<GUI::Label> m_placeholder;
    RefPtr<GUI::Widget> m_hidden_form;
    RefPtr<GUI::TextBox> m_hidden_ssid;
    RefPtr<GUI::TextBox> m_hidden_password;

    ByteString m_filter;
    ByteString m_expanded_ssid;
    Optional<Function<void(Optional<ByteString>)>> m_password_callback;
    Vector<NonnullRefPtr<WifiRow>> m_rows;
    bool m_rebuild_scheduled { false };
};
