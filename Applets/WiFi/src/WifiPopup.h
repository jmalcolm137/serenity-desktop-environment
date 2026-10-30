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
#include <LibCore/Timer.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Widget.h>

#include "NetworkManagerClient.h"

namespace GUI {
class Button;
class CheckBox;
class Label;
class ScrollableContainerWidget;
class TextBox;
}

// What a row says about its network, in the status column.
enum class WifiRowStatus {
    None,
    Connected,
    Connecting,
    Failed,
    Saved,
};

// One network in the list, laid out in fixed columns so every row lines up:
//
//   [signal] SSID ............................ status [lock]
//
// Clicking a row selects it and opens it in place: a line of details and the
// action (Connect / Disconnect), plus a passphrase field when the network needs
// one we do not have. Only one row is open at a time.
class WifiRow final : public GUI::Widget {
    C_OBJECT(WifiRow);

public:
    static constexpr int line_height = 26;
    static constexpr int actions_height = 34;
    static constexpr int options_height = 30;

    virtual ~WifiRow() override = default;

    void set_network(WifiNetwork const&, WifiRowStatus);
    void set_open(bool open, bool wants_password);
    bool is_open() const { return m_open; }
    ByteString const& ssid() const { return m_network.ssid; }
    ByteString password() const;
    void set_password(ByteString const&);
    void focus_password();
    int height_for_state() const { return line_height + (m_open ? actions_height + (m_network.saved ? options_height : 0) : 0); }
    void set_selected(bool);
    bool is_selected() const { return m_selected; }

    Function<void()> on_toggle;
    Function<void()> on_action;
    Function<void()> on_cancel;
    Function<void(ByteString const&)> on_password_edited;
    Function<void(bool)> on_autoconnect_changed;
    Function<void()> on_forget;

private:
    WifiRow();

    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void resize_event(GUI::ResizeEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;
    virtual void keydown_event(GUI::KeyEvent&) override;

    void layout_actions();
    void update_controls();
    void disarm_forget();

    WifiNetwork m_network;
    WifiRowStatus m_status { WifiRowStatus::None };
    bool m_open { false };
    bool m_hovered { false };
    RefPtr<GUI::TextBox> m_password_box;
    RefPtr<GUI::Button> m_action_button;
    RefPtr<GUI::CheckBox> m_autoconnect_box;
    RefPtr<GUI::Button> m_forget_button;
    RefPtr<Core::Timer> m_forget_disarm_timer;
    bool m_forget_armed { false };
    bool m_selected { false };
    bool m_pressed { false };
    bool m_wants_password { false };
};

// The networks popup:
//
//   Wi-Fi                                   [x] Enabled
//   Connected to MockNet
//   -------------------------------------------------
//   | the network list (sunken, scrolls past 8 rows) |
//   -------------------------------------------------
//   [Hidden Network...]              [Network Settings...]
//
// with a search field above the list when there are many networks, and the
// hidden-network form in place of the footer while it is open.
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

    // The popup was shown or hidden.
    void popup_shown();
    void popup_hidden();
    // Put the keyboard back where it was (showing the popup again resets it).
    void restore_focus();

    Function<void()> on_preferred_size_changed;
    // Escape with nothing open asks the host to close the popup.
    Function<void()> on_close_requested;

private:
    WifiPopup();

    virtual void keydown_event(GUI::KeyEvent&) override;

    void build_chrome();
    void rebuild();
    void schedule_rebuild();
    void update_header();
    void update_attempt();
    WifiRowStatus status_for(WifiNetwork const&) const;
    bool wants_password(WifiNetwork const&) const;
    void toggle_row(ByteString const& ssid);
    void row_action(ByteString const& ssid);
    void move_selection(int delta);
    void update_selection();
    WifiNetwork const* network_for(ByteString const& ssid) const;
    void start_attempt(ByteString const& ssid);
    void submit_password();
    void set_hidden_form_visible(bool);
    void connect_hidden();
    void log_layout();

    RefPtr<NetworkManagerClient> m_client;
    RefPtr<GUI::CheckBox> m_enabled_box;
    RefPtr<GUI::Widget> m_header;
    ByteString m_settings_path;
    RefPtr<GUI::TextBox> m_search;
    RefPtr<GUI::ScrollableContainerWidget> m_scroll;
    RefPtr<GUI::Widget> m_list;
    RefPtr<GUI::Label> m_placeholder;
    RefPtr<GUI::Widget> m_footer;
    RefPtr<GUI::Button> m_hidden_button;
    RefPtr<GUI::Button> m_settings_button;
    RefPtr<GUI::Widget> m_hidden_form;
    RefPtr<GUI::TextBox> m_hidden_ssid;
    RefPtr<GUI::TextBox> m_hidden_password;

    ByteString m_filter;
    ByteString m_open_ssid;
    ByteString m_selected_ssid;
    Optional<Function<void(Optional<ByteString>)>> m_password_callback;
    ByteString m_password_ssid;
    ByteString m_password_draft;
    Vector<NonnullRefPtr<WifiRow>> m_rows;
    bool m_rebuild_scheduled { false };

    // The connection the user asked for, until it succeeds or fails.
    ByteString m_attempt_ssid;
    bool m_attempt_seen_connecting { false };
    ByteString m_failed_ssid;
    RefPtr<Core::Timer> m_attempt_timer;
};
