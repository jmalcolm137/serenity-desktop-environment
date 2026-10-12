/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SDE port: the upstream applet reads the SerenityOS kernel's
 * /sys/kernel/net/adapters JSON file. SDE is a Linux environment, so this copy
 * reports the same information from Linux interfaces: getifaddrs() for the IPv4
 * addresses and /sys/class/net/<if>/ for the link state and speed. Everything
 * else (icons, tooltip, notifications, the applet window) is unchanged.
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Notification.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

struct AdapterInfo {
    ByteString name;
    Optional<ByteString> ipv4_address;
};

static ErrorOr<Vector<AdapterInfo>> gather_adapters()
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0)
        return Error::from_errno(errno);

    Vector<AdapterInfo> adapters;
    for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name)
            continue;

        AdapterInfo* adapter = nullptr;
        for (auto& candidate : adapters) {
            if (candidate.name == ifa->ifa_name) {
                adapter = &candidate;
                break;
            }
        }
        if (!adapter) {
            adapters.append({ ifa->ifa_name, {} });
            adapter = &adapters.last();
        }

        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            char buffer[INET_ADDRSTRLEN] = { 0 };
            auto* address = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)))
                adapter->ipv4_address = ByteString(buffer);
        }
    }

    freeifaddrs(ifaddr);
    return adapters;
}

static ByteString read_trimmed(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return StringView(contents.value().data(), contents.value().size()).trim_whitespace().to_byte_string();
}

class NetworkWidget final : public GUI::ImageWidget {
    C_OBJECT_ABSTRACT(NetworkWidget)

public:
    static ErrorOr<NonnullRefPtr<NetworkWidget>> try_create(bool notifications)
    {
        NonnullRefPtr<Gfx::Bitmap> connected_icon = TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/network.png"sv));
        NonnullRefPtr<Gfx::Bitmap> disconnected_icon = TRY(Gfx::Bitmap::load_from_uri("resource://icons/16x16/network-disconnected.png"sv));
        return adopt_nonnull_ref_or_enomem(new (nothrow) NetworkWidget(notifications, move(connected_icon), move(disconnected_icon)));
    }

private:
    NetworkWidget(bool notifications, NonnullRefPtr<Gfx::Bitmap> connected_icon, NonnullRefPtr<Gfx::Bitmap> disconnected_icon)
        : m_connected_icon(move(connected_icon))
        , m_disconnected_icon(move(disconnected_icon))
    {
        m_notifications = notifications;
        update_widget();
        start_timer(5000);
    }

    virtual void timer_event(Core::TimerEvent&) override
    {
        update_widget();
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() != GUI::MouseButton::Primary)
            return;
        GUI::Process::spawn_or_show_error(window(), "/bin/SystemMonitor"sv, Array { "-t", "network" });
    }

    void update_widget()
    {
        auto adapter_info_or_error = get_adapter_info();
        if (adapter_info_or_error.is_error()) {
            dbgln("Couldn't update adapter info: {}", adapter_info_or_error.error());
            set_connected(false);
            m_adapter_info = "No network adapters"_string;
        } else {
            m_adapter_info = adapter_info_or_error.release_value();
        }

        set_tooltip(m_adapter_info);

        if (m_connected)
            NetworkWidget::set_bitmap(m_connected_icon);
        else
            NetworkWidget::set_bitmap(m_disconnected_icon);

        update();
    }

    void notify_on_connect()
    {
        if (!m_notifications)
            return;
        auto notification = GUI::Notification::construct();
        notification->set_title("Network"_string);
        notification->set_icon(m_connected_icon);
        notification->set_text("Network connected"_string);
        notification->show();
    }

    void notify_on_disconnect()
    {
        if (!m_notifications)
            return;
        auto notification = GUI::Notification::construct();
        notification->set_title("Network"_string);
        notification->set_icon(m_disconnected_icon);
        notification->set_text("Network disconnected"_string);
        notification->show();
    }

    void set_connected(bool connected)
    {
        if (m_connected != connected) {
            connected ? notify_on_connect() : notify_on_disconnect();
        }

        m_connected = connected;
    }

    ErrorOr<String> get_adapter_info()
    {
        auto adapters = TRY(gather_adapters());

        StringBuilder adapter_info;
        int connected_adapters = 0;
        for (auto const& adapter : adapters) {
            if (adapter.name == "lo")
                continue;

            auto operstate = read_trimmed(ByteString::formatted("/sys/class/net/{}/operstate", adapter.name));
            auto link_up = operstate == "up"sv || operstate == "unknown"sv;

            auto speed_text = read_trimmed(ByteString::formatted("/sys/class/net/{}/speed", adapter.name));
            int link_speed = StringView(speed_text.characters(), speed_text.length()).to_number<int>().value_or(-1);

            if (adapter.ipv4_address.has_value())
                connected_adapters++;

            if (!adapter_info.is_empty())
                adapter_info.append('\n');

            adapter_info.appendff("{}: {} ", adapter.name, adapter.ipv4_address.value_or(ByteString { "no IP" }));
            if (!link_up)
                adapter_info.appendff("(down)");
            else if (link_speed > 0)
                adapter_info.appendff("({} Mb/s)", link_speed);
            else
                adapter_info.appendff("(up)");
        }

        // show connected icon so long as at least one adapter is connected
        connected_adapters ? set_connected(true) : set_connected(false);

        return adapter_info.to_string();
    }

    String m_adapter_info;
    bool m_connected = false;
    bool m_notifications = true;
    NonnullRefPtr<Gfx::Bitmap> m_connected_icon;
    NonnullRefPtr<Gfx::Bitmap> m_disconnected_icon;
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath unix proc exec"));
    auto app = TRY(GUI::Application::create(arguments));

    TRY(Core::System::unveil("/tmp/session/%sid/portal/notify", "rw"));
    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil("/bin/SystemMonitor", "x"));
    TRY(Core::System::unveil(nullptr, nullptr));

    bool display_notifications = false;
    StringView name;
    Core::ArgsParser args_parser;
    args_parser.add_option(display_notifications, "Display notifications", "display-notifications", 'd');
    args_parser.add_option(name, "Applet name used by WindowServer.ini to set the applet order", "name", 'n', "name");
    args_parser.parse(arguments);

    if (name.is_empty())
        name = "Network"sv;

    auto window = GUI::Window::construct();
    window->set_title(name);
    window->set_window_type(GUI::WindowType::Applet);
    window->set_has_alpha_channel(true);
    window->resize(16, 16);
    auto icon = TRY(NetworkWidget::try_create(display_notifications));
    window->set_main_widget(icon);
    icon->load_from_file("/res/icons/16x16/network.png"sv);
    window->resize(16, 16);
    window->show();

    return app->exec();
}
