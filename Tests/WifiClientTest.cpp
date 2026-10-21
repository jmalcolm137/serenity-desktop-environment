/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// WifiApplet's NetworkManagerClient against Tools/mock-networkmanager on a
// private bus, without the applet's UI or a compositor. Each case starts its own
// bus and mock so it can break them: a slow or stuck NetworkManager, a busy one,
// one that restarts, a bus that goes away, a bus that is not there yet. Beyond
// "the data is right", the property under test is that the applet's only thread
// never waits on NetworkManager: a 5ms ticker runs throughout, and the longest
// gap between its ticks is asserted.
//
// Usage: libwm-wifi-client-test <mock-networkmanager> <scratch directory> [case...]

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibMain/Main.h>

#include "NetworkManagerClient.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;
ByteString g_mock;
ByteString g_scratch;
ByteString g_bus_socket;

void expect(bool condition, ByteString const& what)
{
    if (condition) {
        outln("  ok: {}", what);
    } else {
        outln("  FAIL: {}", what);
        ++g_failures;
    }
}

pid_t spawn(Vector<ByteString> const& argv, ByteString const& log_path)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log_path.characters(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        Vector<char*> args;
        for (auto const& arg : argv)
            args.append(const_cast<char*>(arg.characters()));
        args.append(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    return pid;
}

void stop(pid_t& pid)
{
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    pid = -1;
}

ByteString read_file(ByteString const& path)
{
    int fd = open(path.characters(), O_RDONLY);
    if (fd < 0)
        return {};
    StringBuilder builder;
    char buffer[4096];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        builder.append(StringView { buffer, static_cast<size_t>(n) });
    close(fd);
    return builder.to_byte_string();
}

size_t count_in_file(ByteString const& path, StringView needle)
{
    auto contents = read_file(path);
    size_t count = 0;
    for (size_t at = 0; (at = contents.view().find(needle, at).value_or(contents.length())) < contents.length(); at += needle.length())
        ++count;
    return count;
}

// Pumps the event loop until `done()` or `timeout_ms`, tracking the longest the
// loop went without running the 5ms ticker: the time the thread was stuck.
struct Pump {
    i64 longest_stall_ms { 0 };

    bool until(Function<bool()> done, int timeout_ms)
    {
        auto elapsed = Core::ElapsedTimer::start_new();
        auto last_tick = Core::ElapsedTimer::start_new();
        auto ticker = Core::Timer::create_repeating(5, [&] {
            longest_stall_ms = max(longest_stall_ms, last_tick.elapsed_milliseconds());
            last_tick.start();
        });
        ticker->start();
        while (!done()) {
            if (elapsed.elapsed_milliseconds() > timeout_ms)
                return false;
            Core::EventLoop::current().pump(Core::EventLoop::WaitMode::WaitForEvents);
        }
        longest_stall_ms = max(longest_stall_ms, last_tick.elapsed_milliseconds());
        return true;
    }

    void run_for(int ms)
    {
        auto elapsed = Core::ElapsedTimer::start_new();
        (void)until([&] { return elapsed.elapsed_milliseconds() >= ms; }, ms + 1000);
    }
};

// One private bus and one mock, torn down at the end of the case.
struct Environment {
    pid_t bus { -1 };
    pid_t mock { -1 };
    int mock_generation { 0 };
    ByteString mock_log;

    explicit Environment(bool start_now = true, Vector<ByteString> mock_args = {})
    {
        if (start_now) {
            start_bus();
            start_mock(move(mock_args));
        }
    }
    ~Environment()
    {
        stop(mock);
        stop(bus);
    }

    void start_bus()
    {
        unlink(g_bus_socket.characters());
        bus = spawn({ "dbus-daemon", "--session", "--nofork", "--nopidfile", ByteString::formatted("--address=unix:path={}", g_bus_socket) },
            ByteString::formatted("{}/bus.log", g_scratch));
        for (int i = 0; i < 200; ++i) {
            struct stat st;
            if (stat(g_bus_socket.characters(), &st) == 0)
                return;
            usleep(10'000);
        }
        warnln("dbus-daemon did not come up");
    }

    void start_mock(Vector<ByteString> args = {})
    {
        mock_log = ByteString::formatted("{}/mock-{}-{}.log", g_scratch, getpid(), ++mock_generation);
        unlink(mock_log.characters());
        Vector<ByteString> argv { g_mock };
        argv.extend(move(args));
        mock = spawn(argv, mock_log);
        for (int i = 0; i < 200; ++i) {
            if (count_in_file(mock_log, "serving"sv) > 0)
                return;
            usleep(10'000);
        }
        warnln("the mock did not come up");
    }

    size_t mock_calls(StringView call) const { return count_in_file(mock_log, call); }
};

// Constructing the client counts against the stall budget too: a client that
// talks to NetworkManager synchronously while starting up blocks right here.
NonnullRefPtr<NetworkManagerClient> make_client(Pump& pump)
{
    auto elapsed = Core::ElapsedTimer::start_new();
    auto client = NetworkManagerClient::construct();
    pump.longest_stall_ms = max(pump.longest_stall_ms, elapsed.elapsed_milliseconds());
    return client;
}

bool has_network(NetworkManagerClient const& client, StringView ssid)
{
    for (auto const& network : client.networks()) {
        if (network.ssid == ssid)
            return true;
    }
    return false;
}

Optional<WifiNetwork> network_named(NetworkManagerClient const& client, StringView ssid)
{
    for (auto const& network : client.networks()) {
        if (network.ssid == ssid)
            return network;
    }
    return {};
}

bool shows_mock_networks(NetworkManagerClient const& client)
{
    return client.state().available && client.networks().size() == 2 && client.state().active_ssid == "MockNet"sv;
}

// A stall this long is a blocking call on the applet's thread (the mock's delays
// are 300ms and its stuck call 2s); scheduling noise is far below it.
constexpr i64 stall_budget_ms = 150;

void test_reads_networkmanager()
{
    outln("=== reads NetworkManager ===");
    Environment environment;
    Pump pump;
    auto client = make_client(pump);
    expect(pump.until([&] { return shows_mock_networks(*client); }, 3000), "2 networks, connected to MockNet");
    auto mocknet = network_named(*client, "MockNet"sv);
    auto guest = network_named(*client, "OpenGuest"sv);
    expect(mocknet.has_value() && mocknet->connected && mocknet->strength == 82 && mocknet->secured, "MockNet: connected, 82%, secured");
    expect(mocknet.has_value() && mocknet->saved && mocknet->saved_connection_path == "/org/freedesktop/NetworkManager/Settings/1"sv, "MockNet: its saved profile was found (ListConnections, GetSettings)");
    expect(guest.has_value() && !guest->connected && !guest->saved, "OpenGuest: not connected, not saved");
    expect(!client->networks().is_empty() && client->networks()[0].ssid == "MockNet"sv, "the connected network sorts first");
    expect(client->state().wifi_device_path == "/org/freedesktop/NetworkManager/Devices/1"sv, "the Wi-Fi device is known");
    expect(client->state().wireless_enabled && !client->state().connecting, "wireless on, not connecting");
    expect(environment.mock_calls("AgentManager.Register"sv) == 1, "registered as a secret agent once");
}

void test_slow_networkmanager_does_not_block()
{
    outln("=== a slow NetworkManager does not block the applet ===");
    Environment environment(true, { "--delay-ms", "300" });
    Pump pump;
    auto client = make_client(pump);
    expect(pump.until([&] { return shows_mock_networks(*client); }, 15000), "the refresh completes against a 300ms-per-call NetworkManager");
    expect(pump.longest_stall_ms < stall_budget_ms, ByteString::formatted("the event loop kept running (longest stall {}ms)", pump.longest_stall_ms));
}

void test_stuck_access_point()
{
    outln("=== a call that never returns ===");
    Environment environment(true, { "--hang-ap" });
    Pump pump;
    auto client = make_client(pump);
    // The second access point never answers: the refresh gives up on it after the
    // call timeout and shows what it has.
    bool done = pump.until([&] { return client->state().available && client->networks().size() == 1; }, 6000);
    expect(done, "the refresh completes without the stuck access point");
    expect(has_network(*client, "MockNet"sv) && !has_network(*client, "OpenGuest"sv), "it shows the access point that answered");
    expect(pump.longest_stall_ms < stall_budget_ms, ByteString::formatted("the event loop kept running (longest stall {}ms)", pump.longest_stall_ms));
}

void test_actions()
{
    outln("=== actions reach NetworkManager and their effect is shown ===");
    Environment environment;
    Pump pump;
    auto client = make_client(pump);
    if (!pump.until([&] { return shows_mock_networks(*client); }, 3000)) {
        expect(false, "initial refresh");
        return;
    }

    client->request_scan();
    expect(pump.until([&] { return has_network(*client, "ScanFound"sv); }, 3000), "RequestScan: the scan's new network appears");
    expect(environment.mock_calls("Device.Wireless.RequestScan"sv) == 1, "RequestScan reached NetworkManager once");

    client->disconnect(*network_named(*client, "MockNet"sv));
    expect(pump.until([&] { return client->state().active_ssid.is_empty(); }, 3000), "Disconnect: no longer connected");
    expect(environment.mock_calls("Device.Disconnect"sv) == 1, "Disconnect reached NetworkManager");

    client->set_wireless_enabled(false);
    expect(pump.until([&] { return !client->state().wireless_enabled && client->networks().is_empty(); }, 3000), "Wi-Fi off: no networks");
    expect(environment.mock_calls("Set WirelessEnabled=false"sv) == 1, "SetWirelessEnabled(false) reached NetworkManager");

    // With the radio off there is still a device: a scan asked for now is
    // refused by NetworkManager, and that must not break anything.
    client->request_scan();
    pump.run_for(200);

    client->set_wireless_enabled(true);
    expect(pump.until([&] { return client->state().wireless_enabled && client->networks().size() == 2; }, 3000), "Wi-Fi on: networks are back");
    expect(environment.mock_calls("Set WirelessEnabled=true"sv) == 1, "SetWirelessEnabled(true) reached NetworkManager");

    // A saved, secured network: NetworkManager asks the applet's secret agent.
    ByteString asked_for;
    client->on_password_required = [&](ByteString const& ssid, Function<void(Optional<ByteString>)> answer) {
        asked_for = ssid;
        answer(ByteString { "hunter22" });
    };
    client->connect_to(*network_named(*client, "MockNet"sv), {});
    expect(pump.until([&] { return client->state().active_ssid == "MockNet"sv; }, 3000), "ActivateConnection: connected to MockNet");
    expect(asked_for == "MockNet"sv, "the secret agent asked for MockNet's passphrase");
    expect(environment.mock_calls("secrets psk=hunter22"sv) == 1, "the passphrase reached NetworkManager");

    // The user cancels the prompt: NetworkManager must hear "user canceled".
    client->disconnect(*network_named(*client, "MockNet"sv));
    pump.until([&] { return client->state().active_ssid.is_empty(); }, 3000);
    client->on_password_required = [&](ByteString const&, Function<void(Optional<ByteString>)> answer) { answer({}); };
    client->connect_to(*network_named(*client, "MockNet"sv), {});
    expect(pump.until([&] { return environment.mock_calls("secrets error"sv) == 1; }, 3000), "a cancelled prompt is answered");
    expect(environment.mock_calls("SecretAgent.UserCanceled"sv) == 1, "with SecretAgent.UserCanceled");

    // An unsaved network with a passphrase.
    client->connect_to(*network_named(*client, "OpenGuest"sv), ByteString { "guestpass" });
    expect(pump.until([&] { return environment.mock_calls("NetworkManager.AddAndActivateConnection"sv) == 1; }, 3000), "AddAndActivateConnection reached NetworkManager");
}

void test_busy_networkmanager()
{
    outln("=== a busy NetworkManager: refreshes coalesce, and the latest state wins ===");
    // A change signal every 20ms while each call takes 30ms: a refresh that
    // restarted on every signal would never finish.
    Environment environment(true, { "--delay-ms", "30", "--chatter-ms", "20" });
    Pump pump;
    auto client = make_client(pump);
    size_t completed = 0;
    client->on_refresh_finished = [&] { ++completed; };
    expect(pump.until([&] { return shows_mock_networks(*client); }, 8000), "refreshes still complete");
    client->set_wireless_enabled(false);
    expect(pump.until([&] { return !client->state().wireless_enabled && client->networks().is_empty(); }, 8000), "a change made mid-refresh is shown");
    pump.run_for(500);
    expect(!client->state().wireless_enabled, "and is not overwritten by an older refresh");
    expect(completed >= 2, ByteString::formatted("{} refreshes completed", completed));
    expect(pump.longest_stall_ms < stall_budget_ms, ByteString::formatted("the event loop kept running (longest stall {}ms)", pump.longest_stall_ms));
}

void test_networkmanager_restart()
{
    outln("=== NetworkManager restarts ===");
    Environment environment;
    Pump pump;
    auto client = make_client(pump);
    if (!pump.until([&] { return shows_mock_networks(*client); }, 3000)) {
        expect(false, "initial refresh");
        return;
    }
    auto first_log = environment.mock_log;
    stop(environment.mock);
    expect(pump.until([&] { return !client->state().available; }, 3000), "NetworkManager going away is noticed");
    expect(client->networks().is_empty(), "no stale networks are shown");

    environment.start_mock();
    expect(pump.until([&] { return shows_mock_networks(*client); }, 3000), "NetworkManager coming back is noticed, and its data read");
    expect(pump.until([&] { return environment.mock_calls("AgentManager.Register"sv) == 1; }, 3000), "the secret agent re-registered with the new NetworkManager");
    expect(pump.longest_stall_ms < stall_budget_ms, ByteString::formatted("the event loop kept running (longest stall {}ms)", pump.longest_stall_ms));
}

void test_bus_restart()
{
    outln("=== the system bus restarts ===");
    Environment environment;
    Pump pump;
    auto client = make_client(pump);
    if (!pump.until([&] { return shows_mock_networks(*client); }, 3000)) {
        expect(false, "initial refresh");
        return;
    }
    stop(environment.mock);
    stop(environment.bus);
    expect(pump.until([&] { return !client->state().available; }, 3000), "the dropped bus is noticed");
    pump.run_for(300);

    environment.start_bus();
    environment.start_mock();
    expect(pump.until([&] { return shows_mock_networks(*client); }, 8000), "the applet reconnects and reads NetworkManager again");
    expect(pump.until([&] { return environment.mock_calls("AgentManager.Register"sv) == 1; }, 3000), "and registers its secret agent on the new bus");
    expect(pump.longest_stall_ms < stall_budget_ms, ByteString::formatted("the event loop kept running (longest stall {}ms)", pump.longest_stall_ms));
}

void test_bus_not_up_yet()
{
    outln("=== the system bus is not up when the applet starts ===");
    Environment environment(false);
    unlink(g_bus_socket.characters());
    Pump pump;
    auto client = make_client(pump);
    pump.run_for(300);
    expect(!client->state().available, "unavailable while there is no bus");
    environment.start_bus();
    environment.start_mock();
    expect(pump.until([&] { return shows_mock_networks(*client); }, 8000), "it connects once the bus and NetworkManager are up");
}

void test_destroyed_with_calls_in_flight()
{
    outln("=== the client is destroyed with calls in flight ===");
    Environment environment(true, { "--hang-ap" });
    {
        Pump pump;
        auto client = make_client(pump);
        // Long enough for the refresh to reach the stuck access point.
        pump.run_for(300);
        expect(!client->state().available || client->networks().size() <= 1, "a call is still outstanding");
    }
    // Past the call timeout: a reply or timeout for a destroyed client must not
    // reach it.
    Pump pump;
    pump.run_for(2500);
    expect(true, "nothing ran on the destroyed client");
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    if (arguments.argc < 3) {
        warnln("usage: {} <mock-networkmanager> <scratch directory> [case...]", arguments.strings[0]);
        return 2;
    }
    g_mock = arguments.strings[1];
    g_scratch = arguments.strings[2];
    g_bus_socket = ByteString::formatted("{}/system_bus_socket", g_scratch);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", ByteString::formatted("unix:path={}", g_bus_socket).characters(), 1);
    signal(SIGPIPE, SIG_IGN);

    Core::EventLoop loop;

    struct Case {
        StringView name;
        void (*run)();
    };
    Case const cases[] = {
        { "reads"sv, test_reads_networkmanager },
        { "slow"sv, test_slow_networkmanager_does_not_block },
        { "stuck"sv, test_stuck_access_point },
        { "actions"sv, test_actions },
        { "busy"sv, test_busy_networkmanager },
        { "nm-restart"sv, test_networkmanager_restart },
        { "bus-restart"sv, test_bus_restart },
        { "bus-late"sv, test_bus_not_up_yet },
        { "destroyed"sv, test_destroyed_with_calls_in_flight },
    };
    // Any further arguments pick cases by name.
    for (auto const& test_case : cases) {
        bool selected = arguments.strings.size() == 3;
        for (size_t i = 3; i < arguments.strings.size(); ++i)
            selected |= arguments.strings[i] == test_case.name;
        if (selected)
            test_case.run();
    }

    if (g_failures) {
        outln("==> {} check(s) FAILED", g_failures);
        return 1;
    }
    outln("==> NetworkManagerClient test PASSED");
    return 0;
}
