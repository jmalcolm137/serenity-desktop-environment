/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The <interface> fragment for every impl-portal interface the backends serve.
// Kept verbatim from the interface definitions so introspection matches the
// handlers; tools like `gdbus introspect`, `busctl` and `d-feet` use it (and
// `gdbus call` uses it to type its arguments).

#include "introspection.h"

namespace {

struct InterfaceIntrospection {
    StringView name;
    StringView xml;
};

constexpr InterfaceIntrospection s_interfaces[] = {
    { "org.freedesktop.impl.portal.Access"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Access'>
      <property name='version' type='u' access='read'/>
      <method name='AccessDialog'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='s' name='subtitle' direction='in'/>
      <arg type='s' name='body' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Account"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Account'>
      <method name='GetUserInformation'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.AppChooser"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.AppChooser'>
      <method name='ChooseApplication'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='as' name='choices' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Background"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Background'>
      <method name='SetStatus'>
      <arg type='a{sv}' name='options' direction='in'/>
      </method>
      <method name='SetWallpaperURI'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='uri' direction='in'/>
      <arg type='b' name='show_preview' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      </method>
      <method name='SetAutostart'>
      <arg type='b' name='enable' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      </method>
      <property name='version' type='u' access='read'/>
      <property name='ApplicationName' type='s' access='read'/>
      <property name='ApplicationID' type='s' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.DynamicLauncher"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.DynamicLauncher'>
      <property name='version' type='u' access='read'/>
      <method name='PrepareInstall'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='name' direction='in'/>
      <arg type='v' name='icon_v' direction='in'/>
      <arg type='h' name='icon_fd' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='RequestInstallToken'>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='s' name='token' direction='out'/>
      </method>
      <method name='Launch'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='desktop_file_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Email"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Email'>
      <method name='ComposeEmail'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='address' direction='in'/>
      <arg type='a{sa{sv}}' name='addresses' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.FileChooser"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.FileChooser'>
      <property name='version' type='u' access='read'/>
      <method name='OpenFile'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='SaveFile'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='SaveFiles'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='as' name='files' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.GameMode"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.GameMode'>
      <method name='RegisterGame'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='UnregisterGame'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      </method>
      <method name='QueryStatus'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.GlobalShortcuts"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.GlobalShortcuts'>
      <property name='version' type='u' access='read'/>
      <method name='CreateSession'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='BindShortcuts'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a(sa{sv})' name='shortcuts' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='ListShortcuts'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <signal name='Activated'>
      <arg type='o' name='session_handle'/>
      <arg type='s' name='shortcut_id'/>
      <arg type='t' name='timestamp'/>
      <arg type='a{sv}' name='options'/>
      </signal>
      <signal name='Deactivated'>
      <arg type='o' name='session_handle'/>
      <arg type='s' name='shortcut_id'/>
      <arg type='t' name='timestamp'/>
      <arg type='a{sv}' name='options'/>
      </signal>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.InputCapture"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.InputCapture'>
      <property name='version' type='u' access='read'/>
      <property name='SupportedCapabilities' type='u' access='read'/>
      <method name='CreateSession'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='CreateSession2'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Start'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='GetZones'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='SetPointerBarriers'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='aa{sv}' name='barriers' direction='in'/>
      <arg type='u' name='zone_set' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Enable'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Disable'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Release'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='ConnectToEIS'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='h' name='fd' direction='out'/>
      </method>
      <signal name='Disabled'>
      <arg type='o' name='session_handle' direction='out'/>
      <arg type='a{sv}' name='options' direction='out'/>
      </signal>
      <signal name='Activated'>
      <arg type='o' name='session_handle' direction='out'/>
      <arg type='a{sv}' name='options' direction='out'/>
      </signal>
      <signal name='Deactivated'>
      <arg type='o' name='session_handle' direction='out'/>
      <arg type='a{sv}' name='options' direction='out'/>
      </signal>
      <signal name='ZonesChanged'>
      <arg type='o' name='session_handle' direction='out'/>
      <arg type='a{sv}' name='options' direction='out'/>
      </signal>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Location"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Location'>
      <method name='CreateSession'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Start'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Update'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Stop'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Lockdown"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Lockdown'>
      <property name='disable_application_handlers' type='b' access='read'/>
      <property name='disable_commandline' type='b' access='read'/>
      <property name='disable_printing' type='b' access='read'/>
      <property name='disable_save_to_disk' type='b' access='read'/>
      <property name='disable_location' type='b' access='read'/>
      <property name='disable_user_switching' type='b' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.MemoryMonitor"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.MemoryMonitor'>
      <signal name='LowMemoryWarning'>
      <arg type='u' name='level'/>
      </signal>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Notification"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Notification'>
      <method name='AddNotification'>
      <arg type='s' name='id' direction='in'/>
      <arg type='a{sv}' name='notification' direction='in'/>
      </method>
      <method name='RemoveNotification'>
      <arg type='s' name='id' direction='in'/>
      </method>
      <signal name='ActionInvoked'>
      <arg type='s' name='id'/>
      <arg type='s' name='action'/>
      <arg type='s' name='parameter'/>
      </signal>
      <signal name='NotificationClosed'>
      <arg type='s' name='id'/>
      <arg type='u' name='reason'/>
      </signal>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.OpenURI"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.OpenURI'>
      <method name='OpenURI'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='uri' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='OpenDirectory'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='uri' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='OpenFile'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='h' name='fd' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Activate'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.PowerProfileMonitor"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.PowerProfileMonitor'>
      <property name='version' type='u' access='read'/>
      <property name='power-saver-enabled' type='b' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Print"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Print'>
      <property name='version' type='u' access='read'/>
      <method name='Print'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='h' name='fd' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='PreparePrint'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='title' direction='in'/>
      <arg type='a{sv}' name='settings' direction='in'/>
      <arg type='a{sv}' name='page_setup' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.ProxyResolver"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.ProxyResolver'>
      <method name='Lookup'>
      <arg type='s' name='uri' direction='in'/>
      <arg type='as' name='proxies' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Realtime"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Realtime'>
      <method name='MakeThreadHighPriority'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='i' name='priority' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='MakeThreadRealtime'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='t' name='thread' direction='in'/>
      <arg type='i' name='priority' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.RemoteDesktop"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.RemoteDesktop'>
      <property name='version' type='u' access='read'/>
      <method name='CreateSession'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='SelectDevices'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Start'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='NotifyPointerMotion'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='d' name='dx' direction='in'/>
      <arg type='d' name='dy' direction='in'/>
      </method>
      <method name='NotifyPointerMotionAbsolute'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='stream' direction='in'/>
      <arg type='d' name='x' direction='in'/>
      <arg type='d' name='y' direction='in'/>
      </method>
      <method name='NotifyPointerButton'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='i' name='button' direction='in'/>
      <arg type='u' name='state' direction='in'/>
      </method>
      <method name='NotifyPointerAxis'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='d' name='dx' direction='in'/>
      <arg type='d' name='dy' direction='in'/>
      </method>
      <method name='NotifyPointerAxisDiscrete'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='axis' direction='in'/>
      <arg type='i' name='steps' direction='in'/>
      </method>
      <method name='NotifyKeyboardKeycode'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='i' name='keycode' direction='in'/>
      <arg type='u' name='state' direction='in'/>
      </method>
      <method name='NotifyKeyboardKeysym'>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='i' name='keysym' direction='in'/>
      <arg type='u' name='state' direction='in'/>
      </method>
      <method name='ConnectToEIS'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='h' name='fd' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.ScreenCast"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.ScreenCast'>
      <method name='CreateSession'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='SelectSources'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='Start'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='o' name='session_handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='AvailableSourceTypes'>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='AvailableCursorModes'>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Screenshot"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Screenshot'>
      <property name='version' type='u' access='read'/>
      <method name='Screenshot'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <method name='PickColor'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Secret"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Secret'>
      <property name='version' type='u' access='read'/>
      <method name='RetrieveSecret'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='ay' name='secret' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Settings"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Settings'>
      <method name='Read'>
      <arg type='s' name='namespace' direction='in'/>
      <arg type='s' name='key' direction='in'/>
      <arg type='v' name='value' direction='out'/>
      </method>
      <method name='ReadAll'>
      <arg type='s' name='namespace' direction='in'/>
      <arg type='a{sv}' name='value' direction='out'/>
      </method>
      <signal name='SettingChanged'>
      <arg type='s' name='namespace'/>
      <arg type='s' name='key'/>
      <arg type='v' name='value'/>
      </signal>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Usb"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Usb'>
      <method name='AcquireDevices'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='a(sa{sv}a{sv})' name='devices' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      <property name='version' type='u' access='read'/>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Wallpaper"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Wallpaper'>
      <property name='version' type='u' access='read'/>
      <method name='SetWallpaper'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='uri' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='u' name='response' direction='out'/>
      <arg type='a{sv}' name='results' direction='out'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Inhibit"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Inhibit'>
      <property name='version' type='u' access='read'/>
      <method name='Inhibit'>
      <arg type='o' name='handle' direction='in'/>
      <arg type='s' name='app_id' direction='in'/>
      <arg type='s' name='window' direction='in'/>
      <arg type='u' name='flags' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      </method>
      </interface>)XML"sv },
    { "org.freedesktop.impl.portal.Request"sv,
      R"XML(<interface name='org.freedesktop.impl.portal.Request'>
      <method name='Close'/>
      </interface>)XML"sv },
};

}

Optional<StringView> Portal::interface_introspection_xml(StringView interface_name)
{
    for (auto const& entry : s_interfaces) {
        if (entry.name == interface_name)
            return entry.xml;
    }
    return {};
}
