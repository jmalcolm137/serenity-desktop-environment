/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The SDE AudioServer service: the SerenityOS AudioServer / AudioManager
// protocols, backed by PipeWire.
//
// LibWM spawns it on demand the first time an application connects to
// /tmp/session/<sid>/portal/audio (or audiomanager) and hands over the listening
// sockets (see libgui-wayland's LibWM). Applications that speak the AudioServer
// protocol directly (Piano, SoundPlayer, aplay, the Audio applet) reach it there;
// LibAudio stays a compositor/desktop-agnostic client library that does its own
// output over PulseAudio for clients that use Audio::PlaybackStream.
//
// See docs/06-service-layering.md.

#include "AudioManagerConnection.h"
#include "AudioServerConnection.h"
#include "PipeWireBackend.h"
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibMain/Main.h>
#include <pipewire/pipewire.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    // PipeWire must be initialised once before any pw_* call; the output stream
    // itself starts lazily when a client first plays.
    pw_init(nullptr, nullptr);

    auto server = TRY(Core::LocalServer::try_create());
    TRY(server->take_over_from_system_server("/tmp/session/%sid/portal/audio"sv));
    server->on_accept = [](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        dbgln("AudioServer: audio client connected");
        (void)AudioServer::AudioServerConnection::create(move(client_socket));
    };

    auto manager_server = TRY(Core::LocalServer::try_create());
    TRY(manager_server->take_over_from_system_server("/tmp/session/%sid/portal/audiomanager"sv));
    manager_server->on_accept = [](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        dbgln("AudioServer: manager client connected");
        (void)AudioServer::AudioManagerConnection::create(move(client_socket));
    };

    dbgln("AudioServer: serving the audio and audiomanager portals");
    return event_loop.exec();
}
