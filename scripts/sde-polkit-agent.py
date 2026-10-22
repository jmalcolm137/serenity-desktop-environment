#!/usr/bin/python3
# Copyright (c) 2026, Serenity Desktop Environment contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# sde-polkit-agent — the SDE's graphical PolicyKit authentication agent.
#
# It registers on the system bus for the login session and shows a LibGUI
# password dialog. Polkit's trusted helper performs the actual PAM conversation
# (and reports the result to polkitd), so this process never runs privileged
# code and never handles the PAM stack itself.
#
# The checkbox is real: after a successful authentication it keeps the password
# in this process's memory for the rest of the session and answers subsequent
# PAM prompts without showing the dialog. It is cleared if a cached password is
# rejected, and never written to disk.

import os
import pwd
import shutil
import signal
import subprocess
import sys

import gi

gi.require_version("Polkit", "1.0")
gi.require_version("PolkitAgent", "1.0")

from gi.repository import Gio, GLib, Polkit, PolkitAgent
try:
    from gi.repository import GLibUnix
except ImportError:
    GLibUnix = None

AGENT_PATH = "/org/serenity/SDE/AuthenticationAgent"
AUTH_DIALOG = os.environ.get("SDE_AUTH_DIALOG") or shutil.which("sde-auth-dialog") or "/usr/lib/sde/bin/sde-auth-dialog"
AUTH_XML = """
<node>
  <interface name="org.freedesktop.PolicyKit1.AuthenticationAgent">
    <method name="BeginAuthentication">
      <arg type="s" name="action_id" direction="in"/>
      <arg type="s" name="message" direction="in"/>
      <arg type="s" name="icon_name" direction="in"/>
      <arg type="a{ss}" name="details" direction="in"/>
      <arg type="s" name="cookie" direction="in"/>
      <arg type="a(sa{sv})" name="identities" direction="in"/>
    </method>
    <method name="CancelAuthentication">
      <arg type="s" name="cookie" direction="in"/>
    </method>
  </interface>
</node>
"""


class Agent:
    def __init__(self, bus):
        self.bus = bus
        self.registration = None
        self.interface = None
        self.session = None
        self.cookie = None
        self.invocation = None
        self.cached_password = None
        self.identity = None
        self.username = None
        self.pending_password = None
        self.pending_remember = False

    def register(self):
        node = Gio.DBusNodeInfo.new_for_xml(AUTH_XML)
        self.interface = node.interfaces[0]
        self.registration = self.bus.register_object(
            AGENT_PATH, self.interface, self.on_method_call, None, None
        )
        if not self.registration:
            raise RuntimeError("could not export the authentication agent object")

        session = Polkit.UnixSession.new_for_process_sync(os.getpid())
        if session is None:
            raise RuntimeError("could not resolve the current login session")
        subject = ("unix-session", {"session-id": GLib.Variant("s", session.get_session_id())})
        params = GLib.Variant("((sa{sv})ss)", (subject, "en_US.UTF-8", AGENT_PATH))
        self.bus.call_sync(
            "org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority",
            "org.freedesktop.PolicyKit1.Authority",
            "RegisterAuthenticationAgent",
            params,
            None,
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )
        print("sde-polkit-agent: registered for the current session", flush=True)

    def on_method_call(self, _connection, _sender, _path, _interface, method, params, invocation):
        if method == "BeginAuthentication":
            action_id, message, _icon, _details, cookie, identities = params.unpack()
            self.begin(action_id, message, cookie, identities, invocation)
        elif method == "CancelAuthentication":
            (cookie,) = params.unpack()
            self.cancel(cookie, invocation)
        else:
            invocation.return_dbus_error(
                "org.freedesktop.PolicyKit1.Error.Failed", "unknown method"
            )

    def begin(self, action_id, message, cookie, identities, invocation):
        if self.session is not None:
            invocation.return_dbus_error(
                "org.freedesktop.PolicyKit1.Error.Failed",
                "another authentication is already in progress",
            )
            return

        uid = os.getuid()
        for kind, details in identities:
            if kind != "unix-user":
                continue
            candidate = details.get("uid")
            if isinstance(candidate, GLib.Variant):
                candidate = candidate.unpack()
            if candidate == uid:
                break
            if candidate is not None and uid is None:
                uid = candidate

        self.identity = Polkit.UnixUser.new(int(uid))
        self.username = pwd.getpwuid(int(uid)).pw_name
        self.cookie = cookie
        self.invocation = invocation
        self.pending_password = None
        self.pending_remember = False
        self._message = message

        self.session = PolkitAgent.Session.new(self.identity, cookie)
        self.session.connect("request", self.on_request)
        self.session.connect("completed", self.on_completed)
        self.session.connect("show-error", self.on_show_error)
        self.session.initiate()

    def on_request(self, _session, request, echo_on):
        if echo_on:
            self.session.response(self.username or "")
            return

        if self.cached_password is not None:
            self.pending_password = self.cached_password
            self.session.response(self.cached_password)
            return

        result = prompt(self.username, self._message)
        if result is None:
            self.session.cancel()
            return
        password, remember = result
        self.pending_password = password
        self.pending_remember = remember
        self.session.response(password)

    def on_show_error(self, _session, text):
        print(f"sde-polkit-agent: {text}", file=sys.stderr, flush=True)

    def on_completed(self, _session, gained):
        if gained:
            if self.pending_remember and self.pending_password:
                self.cached_password = self.pending_password
        elif self.pending_password is not None and self.pending_password == self.cached_password:
            # A remembered password stopped working (changed or expired).
            self.cached_password = None

        if self.invocation is not None:
            self.invocation.return_value(None)
        self.session = None
        self.invocation = None
        self.cookie = None
        self.identity = None
        self.pending_password = None
        self.pending_remember = False

    def cancel(self, cookie, invocation):
        if self.session is not None and cookie == self.cookie:
            self.session.cancel()
        invocation.return_value(None)


def prompt(username, message):
    """Run the LibGUI auth dialog. Returns (password, remember) or None."""
    args = [AUTH_DIALOG, "--message", message]
    if username:
        args += ["--username", username]
    try:
        completed = subprocess.run(args, capture_output=True, text=True)
    except OSError as error:
        print(f"sde-polkit-agent: cannot run {AUTH_DIALOG}: {error}", file=sys.stderr)
        return None
    if completed.returncode != 0:
        return None
    lines = completed.stdout.splitlines()
    if not lines:
        return None
    # The dialog writes <password> then <remember 1|0> last. LibWM diagnostics
    # may precede them on stdout, so take the tail rather than the first line.
    remember = len(lines) > 1 and lines[-1].strip() == "1"
    password = lines[-2] if remember else lines[-1]
    return password, remember

def main():
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    agent = Agent(bus)
    try:
        agent.register()
    except GLib.Error as error:
        print(f"sde-polkit-agent: registration failed: {error.message}", file=sys.stderr)
        return 1

    loop = GLib.MainLoop()

    def stop(*_args):
        loop.quit()

    for signal_name in (signal.SIGTERM, signal.SIGINT):
        if GLibUnix is not None:
            GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal_name, stop)
        else:
            GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal_name, stop)

    try:
        loop.run()
    finally:
        if agent.registration is not None:
            bus.unregister_object(agent.registration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
