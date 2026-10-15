/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The SDE backend for the freedesktop USB portal (docs/15). The frontend
// (xdg-desktop-portal) enumerates the connected devices with udev and opens the
// device node on the app's behalf; the *backend* is the broker that decides
// whether the request is allowed — in SerenityOS terms it is the permission
// prompt. This implements org.freedesktop.impl.portal.Usb:
//
//   AcquireDevices(handle o, parent_window s, app_id s,
//                  devices a(sa{sv}a{sv}), options a{sv}) -> (u response, a{sv})
//
// with results.devices = a(sa{sv}) — the subset of the request that was granted,
// each entry being the device id and its access options (writable).
//
// Policy ([Session]-style, environment first):
//   SDE_PORTAL_USB=ask   (default) prompt the user once, then remember the grant
//   SDE_PORTAL_USB=allow grant without asking
//   SDE_PORTAL_USB=deny  refuse (the lockdown behaviour)
// Grants are remembered in usb-permissions in the portal state directory, one
// "<app id>\t<device id>\t<writable>" line each, so a device is only asked about
// once (delete the file, or the line, to revoke).

#include "usb.h"

#include "host.h"
#include "introspection.h"

#include <AK/StringBuilder.h>
#include <LibCore/File.h>

namespace {

constexpr StringView usb_interface = "org.freedesktop.impl.portal.Usb"sv;

struct RequestedDevice {
    ByteString id;
    bool writable { false };
};

ByteString usb_policy()
{
    if (auto policy = Portal::environment("SDE_PORTAL_USB"sv); policy.has_value() && !policy->is_empty())
        return policy.value();
    return ByteString { "ask" };
}

ByteString permissions_path()
{
    return Portal::state_file("usb-permissions"sv);
}

// The remembered grants, as "<app id>\t<device id>\t<writable>" lines.
Optional<ByteString> read_permissions()
{
    auto file = Core::File::open(permissions_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto bytes = contents.release_value();
    return ByteString { StringView { reinterpret_cast<char const*>(bytes.data()), bytes.size() } };
}

bool remembered_grant(StringView app_id, StringView device_id, bool& writable)
{
    auto permissions = read_permissions();
    if (!permissions.has_value())
        return false;
    for (auto line : StringView { permissions.value() }.split_view('\n')) {
        auto parts = line.split_view('\t');
        if (parts.size() != 3)
            continue;
        if (parts[0] == app_id && parts[1] == device_id) {
            writable = parts[2] == "1"sv;
            return true;
        }
    }
    return false;
}

void remember_grant(StringView app_id, StringView device_id, bool writable)
{
    ByteString existing = read_permissions().value_or(ByteString {});
    auto line = ByteString::formatted("{}\t{}\t{}\n", app_id, device_id, writable ? 1 : 0);
    if (StringView { existing }.contains(line))
        return;
    auto file = Core::File::open(permissions_path(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return;
    (void)file.value()->write_until_depleted(existing);
    (void)file.value()->write_until_depleted(line);
}

// The reply path, shared by the synchronous (policy) and asynchronous (dialog)
// paths. The call was taken up front because the dialog may answer later.
void reply_acquire_devices(NonnullRefPtr<Portal::DeferredReply> reply, Vector<RequestedDevice> const& requested, bool granted, StringView app_id, StringView policy)
{
    dbgln("usb: {} {} device(s) for '{}' (policy {})", granted ? "granted" : "refused", requested.size(), app_id, policy);
    Portal::Dictionary results;
    if (!granted) {
        reply->reply_response(to_underlying(Portal::Response::Cancelled), results);
        return;
    }

    // The granted subset: each device's id and its access options.
    Portal::Value::Container granted_devices;
    for (auto const& device : requested) {
        Portal::Dictionary access;
        access.append(Portal::Value::Entry { "writable", Portal::Value::make_bool(device.writable) });
        granted_devices.append(Portal::Value::make_struct({ Portal::Value::make_string(device.id), Portal::Value::make_dict(move(access)) }));
    }
    results.append(Portal::Value::Entry { "devices", Portal::Value::make_array(move(granted_devices), "(sa{sv})"sv) });
    reply->reply_response(to_underlying(Portal::Response::Success), results);
}

void handle_acquire_devices(Portal::MethodCall& call)
{
    auto app_id = call.string_argument(2);
    auto const* devices_argument = call.argument(3);

    Vector<RequestedDevice> requested;
    if (devices_argument && devices_argument->is(Portal::Value::Type::Array)) {
        for (auto const& entry : devices_argument->container()) {
            if (!entry.is(Portal::Value::Type::Struct))
                continue;
            auto const& fields = entry.container();
            // Each element is (id, information a{sv}, access options a{sv}).
            if (fields.size() < 3 || !fields[0].is(Portal::Value::Type::String))
                continue;
            RequestedDevice device;
            device.id = fields[0].as_string();
            device.writable = fields[2].bool_or("writable"sv);
            if (!device.id.is_empty())
                requested.append(move(device));
        }
    }

    if (requested.is_empty()) {
        // Nothing to grant: an empty request is the frontend's bug, not a denial.
        Portal::Dictionary results;
        call.reply_response(to_underlying(Portal::Response::OtherError), results);
        return;
    }

    auto policy = usb_policy();
    auto pending = call.take();

    if (policy == "allow"sv) {
        reply_acquire_devices(pending, requested, true, app_id, policy);
        return;
    }
    if (policy == "deny"sv) {
        reply_acquire_devices(pending, requested, false, app_id, policy);
        return;
    }

    // "ask": honour a remembered grant, else prompt once for all of them.
    bool all_remembered = true;
    for (auto const& device : requested) {
        bool remembered_writable = false;
        if (!remembered_grant(app_id, device.id, remembered_writable) || (device.writable && !remembered_writable))
            all_remembered = false;
    }
    if (all_remembered) {
        reply_acquire_devices(pending, requested, true, app_id, policy);
        return;
    }

    auto dialog = Portal::environment("SDE_PORTAL_ACCESS_DIALOG"sv);
    if (!dialog.has_value() || dialog->is_empty()) {
        dbgln("usb: no SDE_PORTAL_ACCESS_DIALOG; refusing {} device(s) for '{}'", requested.size(), app_id);
        reply_acquire_devices(pending, requested, false, app_id, policy);
        return;
    }

    StringBuilder list;
    for (auto const& device : requested) {
        if (list.length())
            list.append('\n');
        list.append(device.id);
    }

    // The permission prompt is user-interactive: ask it without blocking the
    // backend (docs/15 §6.6), then reply from the continuation.
    auto arguments = Vector<ByteString> { dialog.value(), "-t", ByteString { "Allow USB access?" }, "-s", app_id, "-b", list.to_byte_string() };
    Portal::run_command_async(move(arguments), [pending, requested = move(requested), app_id, policy](Optional<ByteString> output) {
        bool granted = output.has_value() && output->to_number<int>().value_or(1) == 0;
        if (granted) {
            for (auto const& device : requested)
                remember_grant(app_id, device.id, device.writable);
        }
        reply_acquire_devices(pending, requested, granted, app_id, policy);
    });
}

}

void Usb::register_interface(Portal::Service& service)
{
    service.register_interface(
        usb_interface.to_byte_string(),
        [](Portal::MethodCall& call) {
            if (call.member() == "AcquireDevices"sv) {
                handle_acquire_devices(call);
                return;
            }
            call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(1);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(usb_interface).value_or(StringView {}));
}
