/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SDE port: the upstream applet samples the SerenityOS kernel's
 * /sys/kernel/stats, /sys/kernel/memstat and /sys/kernel/net/adapters JSON
 * files. SDE is a Linux environment, so this copy reads the Linux equivalents:
 * /proc/stat for the CPU, /proc/meminfo for the memory, and the per-interface
 * /sys/class/net/<if>/statistics counters for the network. The graph widget,
 * painting and behaviour are unchanged.
 */

#include <AK/CircularQueue.h>
#include <AK/NumberFormat.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Process.h>
#include <LibGUI/Window.h>
#include <LibGfx/Palette.h>
#include <LibMain/Main.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>

enum class GraphType {
    CPU,
    Memory,
    Network,
};

static ErrorOr<String> read_file(StringView path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());
    return String::from_utf8(StringView(contents.data(), contents.size()));
}

// Finds "Key:  <number> ..." in a /proc-style text file and returns <number>.
static Optional<u64> proc_value(StringView text, StringView key)
{
    size_t offset = 0;
    while (offset < text.length()) {
        auto newline = text.find('\n', offset).value_or(text.length());
        auto line = text.substring_view(offset, newline - offset);
        if (line.starts_with(key)) {
            auto colon = line.find(':');
            if (colon.has_value()) {
                auto rest = line.substring_view(*colon + 1).trim_whitespace();
                auto end = rest.find(' ').value_or(rest.length());
                if (auto value = rest.substring_view(0, end).to_number<u64>(); value.has_value())
                    return value.value();
            }
            return {};
        }
        offset = newline + 1;
    }
    return {};
}

class GraphWidget final : public GUI::Frame {
    C_OBJECT(GraphWidget);

public:
    static constexpr size_t history_size = 24;

private:
    GraphWidget(GraphType graph_type, Optional<Gfx::Color> graph_color, Optional<Gfx::Color> graph_error_color)
        : m_graph_type(graph_type)
    {
        set_frame_style(Gfx::FrameStyle::SunkenPanel);
        m_graph_color = graph_color.value_or(palette().menu_selection());
        m_graph_error_color = graph_error_color.value_or(Color::Red);
        start_timer(1000);
    }

    virtual void timer_event(Core::TimerEvent&) override
    {
        switch (m_graph_type) {
        case GraphType::CPU: {
            u64 total, idle;
            if (get_cpu_usage(total, idle)) {
                auto total_diff = total - m_cpu_last_total;
                m_cpu_last_total = total;
                auto idle_diff = idle - m_cpu_last_idle;
                m_cpu_last_idle = idle;
                float cpu = total_diff > 0 ? (float)(total_diff - idle_diff) / (float)total_diff : 0;
                m_history.enqueue(cpu);
                m_tooltip = MUST(String::formatted("CPU usage: {:.1}%", 100 * cpu));
            } else {
                m_history.enqueue(-1);
                m_tooltip = "Unable to determine CPU usage"_string;
            }
            break;
        }
        case GraphType::Memory: {
            u64 allocated, available;
            if (get_memory_usage(allocated, available)) {
                double total_memory = allocated + available;
                double memory = (double)allocated / total_memory;
                m_history.enqueue(memory);
                m_tooltip = MUST(String::formatted("Memory: {} MiB of {:.1} MiB in use", allocated / MiB, total_memory / MiB));
            } else {
                m_history.enqueue(-1);
                m_tooltip = "Unable to determine memory usage"_string;
            }
            break;
        }
        case GraphType::Network: {
            u64 tx {}, rx {};
            if (get_network_usage(tx, rx)) {
                u64 recent_rx = rx - m_network_last_rx;
                m_network_last_rx = rx;
                u64 recent_tx = tx - m_network_last_tx;
                m_network_last_tx = tx;
                if (recent_rx > m_current_scale) {
                    u64 m_old_scale = m_current_scale;
                    // Scale in multiples of 1000 kB/s
                    m_current_scale = (recent_rx / scale_unit) * scale_unit;
                    rescale_history(m_old_scale, m_current_scale);
                } else {
                    // Figure out if we can scale back down.
                    float max = static_cast<float>(recent_rx) / static_cast<float>(m_current_scale);
                    for (auto const value : m_history) {
                        if (value > max)
                            max = value;
                    }
                    if (max < 0.5f && m_current_scale > scale_unit) {
                        u64 m_old_scale = m_current_scale;
                        m_current_scale = ::max((static_cast<u64>(max * m_current_scale) / scale_unit) * scale_unit, scale_unit);
                        rescale_history(m_old_scale, m_current_scale);
                    }
                }
                m_history.enqueue(static_cast<float>(recent_rx) / static_cast<float>(m_current_scale));
                m_tooltip = MUST(String::formatted("Network: Receiving {}/s - Sending {}/s", human_readable_size(recent_rx), human_readable_size(recent_tx)));
            } else {
                m_history.enqueue(-1);
                m_tooltip = "Unable to determine network usage"_string;
            }
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
        set_tooltip(m_tooltip);
        update();
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Frame::paint_event(event);
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        painter.add_clip_rect(frame_inner_rect());
        painter.fill_rect(event.rect(), Color::Black);
        int i = m_history.capacity() - m_history.size();
        auto rect = frame_inner_rect();
        for (auto value : m_history) {
            if (value >= 0) {
                painter.draw_line(
                    { rect.x() + i, rect.bottom() - 1 },
                    { rect.x() + i, rect.top() + (int)(roundf(rect.height() - (value * rect.height()))) },
                    m_graph_color);
            } else {
                painter.draw_line(
                    { rect.x() + i, rect.top() },
                    { rect.x() + i, rect.bottom() - 1 },
                    m_graph_error_color);
            }
            ++i;
        }
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() != GUI::MouseButton::Primary)
            return;
        GUI::Process::spawn_or_show_error(window(), "/bin/SystemMonitor"sv, Array { "-t", m_graph_type == GraphType::Network ? "network" : "graphs" });
    }

    bool get_cpu_usage(u64& total, u64& idle)
    {
        total = 0;
        idle = 0;

        auto contents = read_file("/proc/stat"sv);
        if (contents.is_error())
            return false;

        // The first line is "cpu  user nice system idle iowait irq softirq steal ...".
        StringView text = contents.value();
        auto line_end = text.find('\n').value_or(text.length());
        auto line = text.substring_view(0, line_end);
        if (!line.starts_with("cpu"sv))
            return false;

        unsigned long long user = 0, nice = 0, system = 0, idle_time = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (sscanf(line.to_byte_string().characters(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                &user, &nice, &system, &idle_time, &iowait, &irq, &softirq, &steal)
            < 1)
            return false;

        total = user + nice + system + idle_time + iowait + irq + softirq + steal;
        idle = idle_time + iowait;
        return true;
    }

    bool get_memory_usage(u64& allocated, u64& available)
    {
        auto contents = read_file("/proc/meminfo"sv);
        if (contents.is_error())
            return false;

        auto total_kb = proc_value(contents.value(), "MemTotal:"sv);
        auto available_kb = proc_value(contents.value(), "MemAvailable:"sv);
        if (!total_kb.has_value() || !available_kb.has_value())
            return false;

        auto const total_bytes = total_kb.value() * KiB;
        auto const available_bytes = min(available_kb.value() * KiB, total_bytes);
        allocated = total_bytes - available_bytes;
        available = available_bytes;
        return true;
    }

    bool get_network_usage(u64& tx, u64& rx)
    {
        tx = rx = 0;

        // The interfaces that are up and have an IPv4 address, like the applet's
        // upstream adapter list.
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) < 0)
            return false;

        Vector<ByteString> interfaces;
        for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            ByteString name = ifa->ifa_name;
            if (name == "lo")
                continue;
            bool present = false;
            for (auto const& existing : interfaces) {
                if (existing == name) {
                    present = true;
                    break;
                }
            }
            if (!present)
                interfaces.append(move(name));
        }
        freeifaddrs(ifaddr);

        if (interfaces.is_empty())
            return false;

        for (auto const& name : interfaces) {
            auto operstate = read_file(ByteString::formatted("/sys/class/net/{}/operstate", name));
            if (operstate.is_error())
                continue;
            StringView state = StringView(operstate.value()).trim_whitespace();
            if (state != "up"sv && state != "unknown"sv)
                continue;

            auto rx_bytes = read_file(ByteString::formatted("/sys/class/net/{}/statistics/rx_bytes", name));
            auto tx_bytes = read_file(ByteString::formatted("/sys/class/net/{}/statistics/tx_bytes", name));
            if (rx_bytes.is_error() || tx_bytes.is_error())
                continue;
            rx += rx_bytes.value().to_number<u64>().value_or(0);
            tx += tx_bytes.value().to_number<u64>().value_or(0);
        }

        return tx != 0;
    }

    void rescale_history(u64 old_scale, u64 new_scale)
    {
        float factor = static_cast<float>(old_scale) / static_cast<float>(new_scale);
        for (auto& value : m_history)
            value *= factor;
    }

    GraphType m_graph_type;
    Gfx::Color m_graph_color;
    Gfx::Color m_graph_error_color;
    CircularQueue<float, history_size> m_history;

    u64 m_cpu_last_idle { 0 };
    u64 m_cpu_last_total { 0 };

    u64 m_network_last_rx { 0 };
    u64 m_network_last_tx { 0 };

    static constexpr u64 const scale_unit = 8000;
    u64 m_current_scale { scale_unit };
    String m_tooltip;
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd proc exec rpath unix"));

    auto app = TRY(GUI::Application::create(arguments));

    TRY(Core::System::pledge("stdio recvfd sendfd proc exec rpath"));

    StringView cpu {};
    StringView memory {};
    StringView network {};
    Core::ArgsParser args_parser;
    args_parser.add_option(cpu, "Create CPU graph", "cpu", 'C', "cpu");
    args_parser.add_option(memory, "Create memory graph", "memory", 'M', "memory");
    args_parser.add_option(network, "Create network graph", "network", 'N', "network");
    args_parser.parse(arguments);

    if (cpu.is_empty() && memory.is_empty() && network.is_empty()) {
        printf("At least one of --cpu, --memory, or --network must be used");
        return 1;
    }

    Vector<NonnullRefPtr<GUI::Window>> applet_windows;

    auto create_applet = [&](GraphType graph_type, StringView spec) -> ErrorOr<void> {
        auto parts = spec.split_view(',');

        dbgln("Create applet: {} with spec '{}'", (int)graph_type, spec);

        if (parts.size() != 2)
            return Error::from_string_literal("ResourceGraph: Applet spec is not composed of exactly 2 comma-separated parts");

        auto name = parts[0];
        auto graph_color = Gfx::Color::from_string(parts[1]);

        auto window = GUI::Window::construct();
        window->set_title(name);
        window->set_window_type(GUI::WindowType::Applet);
        window->resize(GraphWidget::history_size + 2, 15);

        auto graph_widget = window->set_main_widget<GraphWidget>(graph_type, graph_color, Optional<Gfx::Color> {});
        window->show();
        applet_windows.append(move(window));

        return {};
    };

    if (!cpu.is_empty())
        TRY(create_applet(GraphType::CPU, cpu));
    if (!memory.is_empty())
        TRY(create_applet(GraphType::Memory, memory));
    if (!network.is_empty())
        TRY(create_applet(GraphType::Network, network));

    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil("/bin/SystemMonitor", "x"));
    TRY(Core::System::unveil(nullptr, nullptr));

    return app->exec();
}
