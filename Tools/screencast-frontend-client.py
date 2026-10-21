#!/usr/bin/env python3
# A ScreenCast client of the real xdg-desktop-portal frontend, for
# scripts/run-screencast-test.sh: it plays the application side of
# org.freedesktop.portal.ScreenCast (CreateSession -> SelectSources -> Start,
# waiting for each Request's Response signal) so the test can check what the
# frontend does with our backend's persist_mode/restore_data.
#
#   screencast-frontend-client.py [--restore-token TOKEN]
#
# Prints one line per result: "start <response> restore_token=<token>".
import argparse
import sys

from gi.repository import Gio, GLib

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
INTERFACE = "org.freedesktop.portal.ScreenCast"

parser = argparse.ArgumentParser()
parser.add_argument("--restore-token", default="")
args = parser.parse_args()

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
sender = bus.get_unique_name()[1:].replace(".", "_")
counter = 0


def request(method, parameters_before, options):
    """Call a Request-returning method and wait for its Response."""
    global counter
    counter += 1
    token = f"sde{counter}"
    path = f"{OBJECT_PATH}/request/{sender}/{token}"
    result = {}
    loop = GLib.MainLoop()

    def on_response(_connection, _sender, _path, _interface, _signal, parameters):
        response, results = parameters.unpack()
        result["response"] = response
        result["results"] = results
        loop.quit()

    subscription = bus.signal_subscribe(BUS_NAME, "org.freedesktop.portal.Request", "Response",
                                        path, None, Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)
    options = dict(options)
    options["handle_token"] = GLib.Variant("s", token)
    bus.call_sync(BUS_NAME, OBJECT_PATH, INTERFACE, method,
                  GLib.Variant(f"({parameters_before[0]}a{{sv}})", (*parameters_before[1], options)),
                  None, Gio.DBusCallFlags.NONE, 5000, None)
    GLib.timeout_add_seconds(10, loop.quit)
    loop.run()
    bus.signal_unsubscribe(subscription)
    if "response" not in result:
        print(f"{method} timed out", flush=True)
        sys.exit(1)
    return result["response"], result["results"]


response, results = request("CreateSession", ("", ()), {"session_handle_token": GLib.Variant("s", "sdesession")})
if response != 0:
    print(f"CreateSession {response}", flush=True)
    sys.exit(1)
session = results["session_handle"]

select_options = {"types": GLib.Variant("u", 1), "persist_mode": GLib.Variant("u", 2)}
if args.restore_token:
    select_options["restore_token"] = GLib.Variant("s", args.restore_token)
response, _ = request("SelectSources", ("o", (session,)), select_options)
if response != 0:
    print(f"SelectSources {response}", flush=True)
    sys.exit(1)

response, results = request("Start", ("os", (session, "")), {})
print(f"start {response} restore_token={results.get('restore_token', '')}", flush=True)
