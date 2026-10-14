/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dbus.h"

#include <AK/StringBuilder.h>
#include <AK/Vector.h>

namespace Portal {

// --- Value ------------------------------------------------------------------

Value Value::make_byte(u8 value)
{
    Value result;
    result.m_type = Type::Byte;
    result.m_byte = value;
    return result;
}

Value Value::make_bool(bool value)
{
    Value result;
    result.m_type = Type::Bool;
    result.m_bool = value;
    return result;
}

Value Value::make_int16(i16 value)
{
    Value result;
    result.m_type = Type::Int16;
    result.m_int16 = value;
    return result;
}

Value Value::make_uint16(u16 value)
{
    Value result;
    result.m_type = Type::UInt16;
    result.m_uint16 = value;
    return result;
}

Value Value::make_int32(i32 value)
{
    Value result;
    result.m_type = Type::Int32;
    result.m_int32 = value;
    return result;
}

Value Value::make_uint32(u32 value)
{
    Value result;
    result.m_type = Type::UInt32;
    result.m_uint32 = value;
    return result;
}

Value Value::make_int64(i64 value)
{
    Value result;
    result.m_type = Type::Int64;
    result.m_int64 = value;
    return result;
}

Value Value::make_uint64(u64 value)
{
    Value result;
    result.m_type = Type::UInt64;
    result.m_uint64 = value;
    return result;
}

Value Value::make_double(double value)
{
    Value result;
    result.m_type = Type::Double;
    result.m_double = value;
    return result;
}

Value Value::make_string(ByteString value)
{
    Value result;
    result.m_type = Type::String;
    result.m_string = move(value);
    return result;
}

Value Value::make_object_path(ByteString value)
{
    Value result;
    result.m_type = Type::ObjectPath;
    result.m_string = move(value);
    return result;
}

Value Value::make_signature(ByteString value)
{
    Value result;
    result.m_type = Type::Signature;
    result.m_string = move(value);
    return result;
}

Value Value::make_unix_fd(int value)
{
    Value result;
    result.m_type = Type::UnixFd;
    result.m_unix_fd = value;
    return result;
}

Value Value::make_array(Container values, ByteString element_signature)
{
    // Catch a mismatched element signature here, with a useful message, rather
    // than letting libdbus abort the process mid-send.
    if (!element_signature.is_empty()) {
        for (auto const& value : values)
            VERIFY(value.signature() == element_signature);
    }
    Value result;
    result.m_type = Type::Array;
    result.m_container = move(values);
    result.m_container_signature = move(element_signature);
    return result;
}

Value Value::make_dict(Dictionary entries, ByteString key_value_signature)
{
    // `a{sv}` is the common shape; make sure every value really is a variant so
    // callers can pass plain values.
    if (key_value_signature.is_empty() || key_value_signature == "sv"sv) {
        for (auto& entry : entries) {
            if (entry.value.type() != Type::Variant)
                entry.value = make_variant(move(entry.value));
        }
    }
    Value result;
    result.m_type = Type::Dict;
    result.m_dictionary = move(entries);
    result.m_container_signature = move(key_value_signature);
    return result;
}

Value Value::make_struct(Container values)
{
    Value result;
    result.m_type = Type::Struct;
    result.m_container = move(values);
    return result;
}

Value Value::make_variant(Value value)
{
    Value result;
    result.m_type = Type::Variant;
    result.m_variant.append(move(value));
    return result;
}

Value const& Value::unwrapped() const
{
    if (m_type == Type::Variant && !m_variant.is_empty())
        return m_variant.first();
    return *this;
}

Value const* Value::find(StringView key) const
{
    for (auto const& entry : m_dictionary) {
        if (entry.key == key)
            return &entry.value;
    }
    return nullptr;
}

ByteString Value::string_or(StringView key, ByteString fallback) const
{
    if (auto const* value = find(key); value && value->unwrapped().is(Type::String))
        return value->unwrapped().as_string();
    return fallback;
}

u32 Value::uint32_or(StringView key, u32 fallback) const
{
    if (auto const* value = find(key); value && value->unwrapped().is(Type::UInt32))
        return value->unwrapped().as_uint32();
    return fallback;
}

i32 Value::int32_or(StringView key, i32 fallback) const
{
    if (auto const* value = find(key); value && value->unwrapped().is(Type::Int32))
        return value->unwrapped().as_int32();
    return fallback;
}

i64 Value::int64_or(StringView key, i64 fallback) const
{
    if (auto const* value = find(key); value) {
        auto const& inner = value->unwrapped();
        if (inner.is(Type::Int64))
            return inner.as_int64();
        if (inner.is(Type::UInt64))
            return static_cast<i64>(inner.as_uint64());
    }
    return fallback;
}

bool Value::bool_or(StringView key, bool fallback) const
{
    if (auto const* value = find(key); value && value->unwrapped().is(Type::Bool))
        return value->unwrapped().as_bool();
    return fallback;
}

double Value::double_or(StringView key, double fallback) const
{
    if (auto const* value = find(key); value && value->unwrapped().is(Type::Double))
        return value->unwrapped().as_double();
    return fallback;
}

static char type_code(Value::Type type)
{
    switch (type) {
    case Value::Type::Byte:
        return DBUS_TYPE_BYTE;
    case Value::Type::Bool:
        return DBUS_TYPE_BOOLEAN;
    case Value::Type::Int16:
        return DBUS_TYPE_INT16;
    case Value::Type::UInt16:
        return DBUS_TYPE_UINT16;
    case Value::Type::Int32:
        return DBUS_TYPE_INT32;
    case Value::Type::UInt32:
        return DBUS_TYPE_UINT32;
    case Value::Type::Int64:
        return DBUS_TYPE_INT64;
    case Value::Type::UInt64:
        return DBUS_TYPE_UINT64;
    case Value::Type::Double:
        return DBUS_TYPE_DOUBLE;
    case Value::Type::String:
        return DBUS_TYPE_STRING;
    case Value::Type::ObjectPath:
        return DBUS_TYPE_OBJECT_PATH;
    case Value::Type::Signature:
        return DBUS_TYPE_SIGNATURE;
    case Value::Type::UnixFd:
        return DBUS_TYPE_UNIX_FD;
    default:
        return DBUS_TYPE_INVALID;
    }
}

ByteString Value::signature() const
{
    switch (m_type) {
    case Type::Array: {
        auto element = m_container_signature;
        if (element.is_empty() && !m_container.is_empty())
            element = m_container.first().signature();
        return ByteString::formatted("a{}", element);
    }
    case Type::Dict: {
        auto key_value = m_container_signature;
        if (key_value.is_empty())
            key_value = "sv";
        return ByteString::formatted("a{{{}}}", key_value);
    }
    case Type::Struct: {
        StringBuilder builder;
        builder.append('(');
        for (auto const& element : m_container)
            builder.append(element.signature());
        builder.append(')');
        return builder.to_byte_string();
    }
    case Type::Variant:
        return "v"sv;
    case Type::Invalid:
        return {};
    default: {
        char code = type_code(m_type);
        return ByteString { StringView { &code, 1 } };
    }
    }
}

Value Value::from_iter(DBusMessageIter* iter)
{
    int type = dbus_message_iter_get_arg_type(iter);
    switch (type) {
    case DBUS_TYPE_VARIANT: {
        DBusMessageIter inner;
        dbus_message_iter_recurse(iter, &inner);
        return make_variant(from_iter(&inner));
    }
    case DBUS_TYPE_ARRAY: {
        int element_type = dbus_message_iter_get_element_type(iter);
        DBusMessageIter elements;
        dbus_message_iter_recurse(iter, &elements);
        if (element_type == DBUS_TYPE_DICT_ENTRY) {
            Dictionary entries;
            ByteString key_value_signature;
            while (dbus_message_iter_get_arg_type(&elements) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&elements, &entry);
                ByteString key;
                char const* key_text = nullptr;
                dbus_message_iter_get_basic(&entry, &key_text);
                if (key_text)
                    key = key_text;
                dbus_message_iter_next(&entry);
                auto value = from_iter(&entry);
                if (key_value_signature.is_empty())
                    key_value_signature = ByteString::formatted("s{}", value.signature());
                entries.append(Value::Entry { move(key), move(value) });
                dbus_message_iter_next(&elements);
            }
            return make_dict(move(entries), key_value_signature);
        }
        Container values;
        ByteString element_signature;
        while (dbus_message_iter_get_arg_type(&elements) == element_type) {
            auto value = from_iter(&elements);
            if (element_signature.is_empty())
                element_signature = value.signature();
            values.append(move(value));
            dbus_message_iter_next(&elements);
        }
        return make_array(move(values), element_signature);
    }
    case DBUS_TYPE_STRUCT: {
        DBusMessageIter fields;
        dbus_message_iter_recurse(iter, &fields);
        Container values;
        while (dbus_message_iter_get_arg_type(&fields) != DBUS_TYPE_INVALID) {
            values.append(from_iter(&fields));
            dbus_message_iter_next(&fields);
        }
        return make_struct(move(values));
    }
    default:
        break;
    }

    switch (type) {
    case DBUS_TYPE_BYTE: {
        unsigned char value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_byte(value);
    }
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_bool(value);
    }
    case DBUS_TYPE_INT16: {
        dbus_int16_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_int16(value);
    }
    case DBUS_TYPE_UINT16: {
        dbus_uint16_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_uint16(value);
    }
    case DBUS_TYPE_INT32: {
        dbus_int32_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_int32(value);
    }
    case DBUS_TYPE_UINT32: {
        dbus_uint32_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_uint32(value);
    }
    case DBUS_TYPE_INT64: {
        dbus_int64_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_int64(value);
    }
    case DBUS_TYPE_UINT64: {
        dbus_uint64_t value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_uint64(value);
    }
    case DBUS_TYPE_DOUBLE: {
        double value = 0;
        dbus_message_iter_get_basic(iter, &value);
        return make_double(value);
    }
    case DBUS_TYPE_STRING: {
        char const* value = nullptr;
        dbus_message_iter_get_basic(iter, &value);
        return make_string(value ? ByteString { value } : ByteString {});
    }
    case DBUS_TYPE_OBJECT_PATH: {
        char const* value = nullptr;
        dbus_message_iter_get_basic(iter, &value);
        return make_object_path(value ? ByteString { value } : ByteString {});
    }
    case DBUS_TYPE_SIGNATURE: {
        char const* value = nullptr;
        dbus_message_iter_get_basic(iter, &value);
        return make_signature(value ? ByteString { value } : ByteString {});
    }
    case DBUS_TYPE_UNIX_FD: {
        int value = -1;
        dbus_message_iter_get_basic(iter, &value);
        return make_unix_fd(value);
    }
    default:
        return {};
    }
}

void Value::append_to(DBusMessageIter* parent) const
{
    switch (m_type) {
    case Type::Array: {
        auto element_signature = m_container_signature;
        if (element_signature.is_empty() && !m_container.is_empty())
            element_signature = m_container.first().signature();
        DBusMessageIter array;
        dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, element_signature.characters(), &array);
        for (auto const& element : m_container)
            element.append_to(&array);
        dbus_message_iter_close_container(parent, &array);
        return;
    }
    case Type::Dict: {
        auto key_value = m_container_signature;
        if (key_value.is_empty())
            key_value = "sv";
        auto array_signature = ByteString::formatted("{{{}}}", key_value);
        DBusMessageIter array;
        dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, array_signature.characters(), &array);
        for (auto const& entry : m_dictionary) {
            DBusMessageIter dict_entry;
            dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &dict_entry);
            char const* key = entry.key.characters();
            dbus_message_iter_append_basic(&dict_entry, DBUS_TYPE_STRING, &key);
            entry.value.append_to(&dict_entry);
            dbus_message_iter_close_container(&array, &dict_entry);
        }
        dbus_message_iter_close_container(parent, &array);
        return;
    }
    case Type::Struct: {
        DBusMessageIter structure;
        dbus_message_iter_open_container(parent, DBUS_TYPE_STRUCT, nullptr, &structure);
        for (auto const& element : m_container)
            element.append_to(&structure);
        dbus_message_iter_close_container(parent, &structure);
        return;
    }
    case Type::Variant: {
        auto const& inner = m_variant.is_empty() ? Value {} : m_variant.first();
        auto signature = inner.signature();
        DBusMessageIter variant;
        dbus_message_iter_open_container(parent, DBUS_TYPE_VARIANT, signature.characters(), &variant);
        inner.append_to(&variant);
        dbus_message_iter_close_container(parent, &variant);
        return;
    }
    default:
        break;
    }

    auto code = type_code(m_type);
    switch (code) {
    case DBUS_TYPE_BYTE: {
        unsigned char value = m_byte;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t value = m_bool;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_INT16: {
        dbus_int16_t value = m_int16;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_UINT16: {
        dbus_uint16_t value = m_uint16;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_INT32: {
        dbus_int32_t value = m_int32;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_UINT32: {
        dbus_uint32_t value = m_uint32;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_INT64: {
        dbus_int64_t value = m_int64;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_UINT64: {
        dbus_uint64_t value = m_uint64;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_DOUBLE: {
        double value = m_double;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_STRING: {
        char const* value = m_string.characters();
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_OBJECT_PATH: {
        char const* value = m_string.characters();
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_SIGNATURE: {
        char const* value = m_string.characters();
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    case DBUS_TYPE_UNIX_FD: {
        int value = m_unix_fd;
        dbus_message_iter_append_basic(parent, code, &value);
        break;
    }
    default:
        break;
    }
}

// --- MethodCall -------------------------------------------------------------

MethodCall::MethodCall(DBusConnection* connection, DBusMessage* message)
    : m_connection(connection)
    , m_message(message)
{
    if (auto const* interface = dbus_message_get_interface(message))
        m_interface = interface;
    if (auto const* member = dbus_message_get_member(message))
        m_member = member;
    if (auto const* path = dbus_message_get_path(message))
        m_path = path;
    if (auto const* sender = dbus_message_get_sender(message))
        m_sender = sender;

    DBusMessageIter iter;
    if (dbus_message_iter_init(message, &iter)) {
        while (true) {
            m_arguments.append(Value::from_iter(&iter));
            if (!dbus_message_iter_next(&iter))
                break;
        }
    }
}

Value const* MethodCall::argument(size_t index) const
{
    if (index >= m_arguments.size())
        return nullptr;
    return &m_arguments[index];
}

ByteString MethodCall::string_argument(size_t index) const
{
    // Accept an object path or a signature where a string is expected: a client
    // that does not introspect us (e.g. `gdbus call`) may send a `s` for an `o`
    // argument, and vice versa.
    if (auto const* value = argument(index); value && (value->is(Value::Type::String) || value->is(Value::Type::ObjectPath)))
        return value->as_string();
    return {};
}

ByteString MethodCall::object_path_argument(size_t index) const
{
    if (auto const* value = argument(index); value && (value->is(Value::Type::ObjectPath) || value->is(Value::Type::String)))
        return value->as_object_path();
    return {};
}

void MethodCall::reply(Vector<Value> const& results)
{
    if (m_replied)
        return;
    m_replied = true;
    auto* reply = dbus_message_new_method_return(m_message);
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    for (auto const& value : results)
        value.append_to(&iter);
    dbus_connection_send(m_connection, reply, nullptr);
    dbus_connection_flush(m_connection);
    dbus_message_unref(reply);
}

void MethodCall::reply_response(u32 response, Dictionary const& results)
{
    Dictionary copy;
    copy.ensure_capacity(results.size());
    for (auto const& entry : results)
        copy.append(Value::Entry { entry.key, entry.value });
    reply(Vector<Value> { Value::make_uint32(response), Value::make_dict(move(copy)) });
}

void MethodCall::reply_error(StringView name, StringView message)
{
    if (m_replied)
        return;
    m_replied = true;
    auto* reply = dbus_message_new_error(m_message, name.to_byte_string().characters(), message.to_byte_string().characters());
    if (!reply)
        return;
    dbus_connection_send(m_connection, reply, nullptr);
    dbus_connection_flush(m_connection);
    dbus_message_unref(reply);
}

// --- Service ----------------------------------------------------------------

Service::Service(DBusConnection* connection, ByteString bus_name, ByteString object_path)
    : m_connection(connection)
    , m_bus_name(move(bus_name))
    , m_object_path(move(object_path))
{
}

Service::~Service()
{
    if (m_connection) {
        dbus_connection_remove_filter(m_connection, &Service::filter, this);
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
    }
}

ErrorOr<NonnullRefPtr<Service>> Service::create(ByteString bus_name, ByteString object_path)
{
    DBusError error;
    dbus_error_init(&error);

    auto* connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!connection) {
        if (dbus_error_is_set(&error)) {
            dbgln("portal: no session bus: {} ({})", error.name, error.message);
            dbus_error_free(&error);
        }
        return Error::from_string_literal("no session bus");
    }
    dbus_connection_set_exit_on_disconnect(connection, false);

    auto request = dbus_bus_request_name(connection, bus_name.characters(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return Error::from_string_literal("could not own bus name");
    }
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        return Error::from_string_literal("bus name already owned");

    auto service = adopt_ref(*new Service(connection, move(bus_name), move(object_path)));
    dbus_connection_add_filter(connection, &Service::filter, service.ptr(), nullptr);

    int fd = -1;
    if (dbus_connection_get_unix_fd(connection, &fd) && fd >= 0) {
        service->m_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read, service.ptr());
        service->m_notifier->on_activation = [ptr = service.ptr()] {
            ptr->on_socket_readable();
        };
    }

    return service;
}

Service::Interface* Service::interface_named(StringView name)
{
    for (auto& interface : m_interfaces) {
        if (interface.name == name)
            return &interface;
    }
    return nullptr;
}

void Service::register_interface(ByteString name, MethodHandler method_handler, PropertyHandler property_handler, Vector<ByteString> property_names)
{
    m_interfaces.append({ move(name), move(method_handler), move(property_handler), move(property_names) });
}

void Service::emit_signal(StringView interface, StringView name, Vector<Value> const& arguments)
{
    auto* signal = dbus_message_new_signal(m_object_path.characters(), interface.to_byte_string().characters(), name.to_byte_string().characters());
    if (!signal)
        return;
    DBusMessageIter iter;
    dbus_message_iter_init_append(signal, &iter);
    for (auto const& value : arguments)
        value.append_to(&iter);
    dbus_connection_send(m_connection, signal, nullptr);
    dbus_connection_flush(m_connection);
    dbus_message_unref(signal);
}

DBusHandlerResult Service::filter(DBusConnection*, DBusMessage* message, void* user_data)
{
    auto* service = static_cast<Service*>(user_data);
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    auto const* path = dbus_message_get_path(message);
    if (!path || service->m_object_path != path)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    service->handle_message(message);
    return DBUS_HANDLER_RESULT_HANDLED;
}

void Service::handle_message(DBusMessage* message)
{
    MethodCall call { m_connection, message };
    auto const& interface = call.interface();

    if (interface == "org.freedesktop.DBus.Peer"sv) {
        if (call.member() == "Ping"sv)
            call.reply();
        else if (call.member() == "GetMachineId"sv)
            call.reply(Vector<Value> { Value::make_string("00000000000000000000000000000000") });
        else
            call.reply_error(ErrorName::UnknownMethod, "unknown Peer method"sv);
        return;
    }

    if (interface == "org.freedesktop.DBus.Properties"sv) {
        if (call.member() == "Get"sv) {
            auto target = call.string_argument(0);
            auto property = call.string_argument(1);
            if (auto* registered = interface_named(target)) {
                if (registered->property_handler) {
                    if (auto value = registered->property_handler(property); value.has_value()) {
                        call.reply(Vector<Value> { Value::make_variant(value.release_value()) });
                        return;
                    }
                }
            }
            call.reply_error(ErrorName::UnknownProperty, "unknown property"sv);
            return;
        }
        if (call.member() == "GetAll"sv) {
            auto target = call.string_argument(0);
            Value::Dictionary all;
            if (auto* registered = interface_named(target); registered && registered->property_handler) {
                for (auto const& property : registered->property_names) {
                    if (auto value = registered->property_handler(property); value.has_value())
                        all.append({ property, Value::make_variant(value.release_value()) });
                }
            }
            call.reply(Vector<Value> { Value::make_dict(move(all)) });
            return;
        }
        call.reply_error(ErrorName::UnknownMethod, "unknown Properties method"sv);
        return;
    }

    if (auto* registered = interface_named(interface)) {
        if (!registered->method_handler) {
            call.reply_error(ErrorName::NotSupported, "interface is read-only"sv);
            return;
        }
        registered->method_handler(call);
        if (!call.m_replied)
            call.reply_error(ErrorName::UnknownMethod, "unhandled method"sv);
        return;
    }

    call.reply_error(ErrorName::UnknownInterface, "unknown interface"sv);
}

void Service::on_socket_readable()
{
    if (!m_connection)
        return;
    dbus_connection_read_write(m_connection, 0);
    while (dbus_connection_get_dispatch_status(m_connection) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(m_connection);
}

}
