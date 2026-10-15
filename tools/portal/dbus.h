/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A small, Serenity-style D-Bus service layer for the portal backends.
//
// The portal backends are ordinary host D-Bus services, so they need a D-Bus
// transport; like the Wi-Fi applet we use the reference C library (`dbus-1`) for
// the wire and wrap it in AK/LibCore — a `Core::Notifier` drives the connection,
// values are AK types, and there is no GLib/GIO in the tree. This header is the
// AK-side counterpart of GVariant/GDBus: `Portal::Value` is the value model and
// `Portal::Service` owns the connection, the bus name and the dispatch loop.

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>

#include <dbus/dbus.h>

namespace Portal {

// A D-Bus value. It covers the subset the portal interfaces use: the basic
// types, arrays, dictionaries (`a{sv}`), structs and variants. Containers carry
// an explicit signature so an empty array (e.g. ProxyResolver's `[]` or an empty
// device list) still marshals to the right D-Bus type.
class Value {
public:
    enum class Type {
        Invalid,
        Byte,
        Bool,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Double,
        String,
        ObjectPath,
        Signature,
        UnixFd,
        Array,
        Dict,
        Struct,
        Variant,
    };

    using Container = Vector<Value>;

    struct Entry;
    using Dictionary = Vector<Entry>;

    Value() = default;

    static Value make_byte(u8);
    static Value make_bool(bool);
    static Value make_int16(i16);
    static Value make_uint16(u16);
    static Value make_int32(i32);
    static Value make_uint32(u32);
    static Value make_int64(i64);
    static Value make_uint64(u64);
    static Value make_double(double);
    static Value make_string(ByteString);
    static Value make_object_path(ByteString);
    static Value make_signature(ByteString);
    static Value make_unix_fd(int);
    static Value make_array(Container, ByteString element_signature = {});
    static Value make_dict(Dictionary, ByteString key_value_signature = "sv");
    static Value make_struct(Container);
    static Value make_variant(Value);

    Type type() const { return m_type; }
    bool is(Type type) const { return m_type == type; }
    bool is_valid() const { return m_type != Type::Invalid; }

    u8 as_byte() const { return m_byte; }
    bool as_bool() const { return m_bool; }
    i16 as_int16() const { return m_int16; }
    u16 as_uint16() const { return m_uint16; }
    i32 as_int32() const { return m_int32; }
    u32 as_uint32() const { return m_uint32; }
    i64 as_int64() const { return m_int64; }
    u64 as_uint64() const { return m_uint64; }
    double as_double() const { return m_double; }
    int as_unix_fd() const { return m_unix_fd; }
    ByteString const& as_string() const { return m_string; }
    ByteString const& as_object_path() const { return m_string; }

    // The unwrapped value of a variant (or the value itself when it is not one).
    Value const& unwrapped() const;

    Container const& container() const { return m_container; }
    Dictionary const& dictionary() const { return m_dictionary; }

    Value const* find(StringView key) const;
    bool has_key(StringView key) const { return find(key) != nullptr; }

    // Convenience typed lookups over a dictionary value.
    ByteString string_or(StringView key, ByteString fallback = {}) const;
    u32 uint32_or(StringView key, u32 fallback = 0) const;
    i32 int32_or(StringView key, i32 fallback = 0) const;
    i64 int64_or(StringView key, i64 fallback = 0) const;
    bool bool_or(StringView key, bool fallback = false) const;
    double double_or(StringView key, double fallback = 0.0) const;

    ByteString signature() const;

    // Reads the value at `iter` without advancing it.
    static Value from_iter(DBusMessageIter* iter);
    // Appends this value to an open container (or the top-level message).
    void append_to(DBusMessageIter* parent) const;

private:
    Type m_type { Type::Invalid };
    u8 m_byte { 0 };
    bool m_bool { false };
    i16 m_int16 { 0 };
    u16 m_uint16 { 0 };
    i32 m_int32 { 0 };
    u32 m_uint32 { 0 };
    i64 m_int64 { 0 };
    u64 m_uint64 { 0 };
    double m_double { 0 };
    int m_unix_fd { -1 };
    ByteString m_string;
    ByteString m_container_signature;
    Container m_container;
    Dictionary m_dictionary;
    Container m_variant;
};

// A dictionary entry: `a{sv}` and friends are always string-keyed in the portal
// interfaces. Defined after `Value` so it can hold one by value.
struct Value::Entry {
    ByteString key;
    Value value;
};

using Dictionary = Value::Dictionary;

// Defined after MethodCall (a call can be taken from its handler, see
// MethodCall::take()).
class DeferredReply;

// An incoming method call: the parsed arguments plus reply helpers.
class MethodCall {
public:
    ByteString const& interface() const { return m_interface; }
    ByteString const& member() const { return m_member; }
    ByteString const& path() const { return m_path; }
    ByteString const& sender() const { return m_sender; }

    // The call's arguments. `argument(index)` is null when absent.
    Vector<Value> const& arguments() const { return m_arguments; }
    Value const* argument(size_t index) const;
    ByteString string_argument(size_t index) const;
    ByteString object_path_argument(size_t index) const;

    // The handler will finish asynchronously (an external helper is running): take
    // the call so it outlives the handler, and reply through the returned handle.
    // The Service then leaves the call unanswered.
    NonnullRefPtr<DeferredReply> take();

    // Replies. Exactly one of these must be called.
    void reply(Vector<Value> const& results = {});
    // The `(u a{sv})` reply the `impl.portal` interfaces use.
    void reply_response(u32 response, Dictionary const& results = {});
    void reply_error(StringView name, StringView message);

    Dictionary const& results() const { return m_results; }
    Dictionary& results() { return m_results; }

private:
    friend class Service;
    explicit MethodCall(DBusConnection* connection, DBusMessage* message);

    DBusConnection* m_connection { nullptr };
    DBusMessage* m_message { nullptr };
    ByteString m_interface;
    ByteString m_member;
    ByteString m_path;
    ByteString m_sender;
    Vector<Value> m_arguments;
    bool m_replied { false };
    bool m_deferred { false };
    Dictionary m_results;
};

// Standard `org.freedesktop.impl.portal` responses (the `u` in `(u a{sv})`).
enum class Response : u32 {
    Success = 0,
    Cancelled = 1,
    OtherError = 2,
};

// The reply to a call that was taken from a handler (MethodCall::take()): an
// asynchronous helper is running and the handler has returned. Copyable, so the
// continuation that produces the reply can capture it.
class DeferredReply : public RefCounted<DeferredReply> {
public:
    static NonnullRefPtr<DeferredReply> create(DBusConnection* connection, DBusMessage* message)
    {
        return adopt_ref(*new DeferredReply(connection, message));
    }

    ~DeferredReply();

    void reply(Vector<Value> const& results = {});
    void reply_response(u32 response, Dictionary const& results = {});
    void reply_error(StringView name, StringView message);

private:
    DeferredReply(DBusConnection*, DBusMessage*);

    DBusConnection* m_connection { nullptr };
    DBusMessage* m_message { nullptr };
    bool m_replied { false };
};

namespace ErrorName {
// `org.freedesktop.DBus.Error.*` names the portals return for bad calls.
constexpr StringView Failed = "org.freedesktop.DBus.Error.Failed"sv;
constexpr StringView InvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs"sv;
constexpr StringView UnknownMethod = "org.freedesktop.DBus.Error.UnknownMethod"sv;
constexpr StringView UnknownProperty = "org.freedesktop.DBus.Error.UnknownProperty"sv;
constexpr StringView UnknownInterface = "org.freedesktop.DBus.Error.UnknownInterface"sv;
constexpr StringView NotSupported = "org.freedesktop.DBus.Error.NotSupported"sv;
}

// A service connection that owns a well-known bus name and an object path, and
// routes method calls and property reads to the registered interfaces.
class Service : public Core::EventReceiver {
    C_OBJECT(Service);

public:
    // Connects to the session bus, owns `bus_name`, and serves `object_path`
    // (defaults to /org/freedesktop/portal/desktop).
    static ErrorOr<NonnullRefPtr<Service>> create(ByteString bus_name, ByteString object_path);

    virtual ~Service() override;

    ByteString const& bus_name() const { return m_bus_name; }
    ByteString const& object_path() const { return m_object_path; }
    DBusConnection* connection() const { return m_connection; }

    using MethodHandler = Function<void(MethodCall&)>;
    // Returns the property value, or an empty Optional for an unknown property.
    using PropertyHandler = Function<Optional<Value>(StringView property)>;

    // `introspection_xml` is the interface's `<interface>` fragment, served over
    // org.freedesktop.DBus.Introspectable.
    void register_interface(ByteString name, MethodHandler, PropertyHandler = {}, Vector<ByteString> property_names = {}, StringView introspection_xml = {});

    // Object paths served besides the service's own: the request objects the impl
    // portals export (org.freedesktop.impl.portal.Request) live at the handle path
    // the caller passes in, not at the service's path. Dispatch is by interface.
    void add_object_path(ByteString path);
    void remove_object_path(StringView path);

    // Emits a signal on `interface`. `arguments` are appended in order.
    void emit_signal(StringView interface, StringView name, Vector<Value> const& arguments = {});
    // Emits a signal on an arbitrary object path: the request objects the impl
    // portals export (see org.freedesktop.impl.portal.Request) live outside the
    // service's own path.
    void emit_signal_on(StringView path, StringView interface, StringView name, Vector<Value> const& arguments = {});

private:
    Service(DBusConnection*, ByteString bus_name, ByteString object_path);

    struct Interface {
        ByteString name;
        MethodHandler method_handler;
        PropertyHandler property_handler;
        Vector<ByteString> property_names;
        StringView introspection_xml;
    };

    static DBusHandlerResult filter(DBusConnection*, DBusMessage*, void*);
    void on_socket_readable();
    void handle_message(DBusMessage*);
    void handle_introspect(MethodCall&);

    Interface* interface_named(StringView name);
    bool owns_path(StringView path) const;

    DBusConnection* m_connection { nullptr };
    ByteString m_bus_name;
    ByteString m_object_path;
    Vector<ByteString> m_object_paths;
    Vector<Interface> m_interfaces;
    RefPtr<Core::Notifier> m_notifier;
};

}
