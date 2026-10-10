/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The SDE session LaunchServer.
//
// When SDE is running, an application's launch portal connects *here* instead of
// to the in-process fallback inside libgui-wayland. See
// docs/06-service-layering.md: libgui-wayland stays compositor-agnostic (it keeps
// the in-process fallback), while the session owns the shared service.
//
// It reuses the same handler resolution (Launch::HandlerDatabase) and the same
// protocol implementation (LibWM::LaunchServerConnection) as the fallback, so
// behaviour is identical whether or not SDE is present.
//
// The applications' host directory must be given via SDE_APP_BIN_DIR: they are
// not built next to this service.

#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

#include "LaunchServerConnection.h"

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop loop;

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/launch"sv));

    // The session runtime directory may not exist yet, and a crashed predecessor
    // may have left a socket file behind.
    TRY(Core::Directory::create(LexicalPath(path).dirname(), Core::Directory::CreateDirectories::Yes));
    (void)Core::System::unlink(path);

    auto server = TRY(Core::LocalServer::try_create());

    // Keep the accepted connections alive for the lifetime of the service.
    static Vector<NonnullRefPtr<LibWM::LaunchServerConnection>> connections;
    server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> socket) {
        dbgln("SDE LaunchServer: client connected");
        connections.append(LibWM::LaunchServerConnection::create(move(socket)));
    };

    if (!server->listen(path))
        return Error::from_string_literal("SDE LaunchServer: could not listen on the launch portal");
    dbgln("SDE LaunchServer: listening on '{}'", path);

    return loop.exec();
}
