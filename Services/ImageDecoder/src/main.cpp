/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The SDE session ImageDecoder service.
//
// When SDE is running, an application's image portal connects *here* instead of
// to the in-process fallback inside libgui-wayland. See
// docs/06-service-layering.md.
//
// It reuses SerenityOS's own ImageDecoder::ConnectionFromClient, so decoding
// behaves identically whether or not SDE is present. As a separate process it
// can keep the class's normal background decode job (a shared in-process host
// cannot; see the layering doc).

#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <ImageDecoder/ConnectionFromClient.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto path = TRY(Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/image"sv));

    // The session runtime directory may not exist yet, and a crashed predecessor
    // may have left a socket file behind.
    TRY(Core::Directory::create(LexicalPath(path).dirname(), Core::Directory::CreateDirectories::Yes));
    (void)Core::System::unlink(path);

    auto server = TRY(Core::LocalServer::try_create());

    // Keep the accepted connections alive for the lifetime of the service.
    static Vector<NonnullRefPtr<ImageDecoder::ConnectionFromClient>> clients;
    server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> socket) {
        dbgln("SDE ImageDecoder: client connected");
        auto connection = ImageDecoder::ConnectionFromClient::construct(move(socket));
        // This service serves many clients; one disconnecting must not shut the
        // service down (the standalone decoder exits with its single client).
        connection->set_exit_when_client_disconnects(false);
        clients.append(move(connection));
    };

    if (!server->listen(path))
        return Error::from_string_literal("SDE ImageDecoder: could not listen on the image portal");
    dbgln("SDE ImageDecoder: listening on '{}'", path);

    return event_loop.exec();
}
