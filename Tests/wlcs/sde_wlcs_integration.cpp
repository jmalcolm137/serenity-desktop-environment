/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WLCS (Wayland Conformance Test Suite) integration for SDE's WindowServer.
 *
 * WLCS expects a compositor integration module, not a test protocol. SDE's
 * compositor is an executable rather than a reusable library, so this module
 * starts it headless as a child process, waits for its Wayland socket, and
 * hands WLCS a connected client socket.
 *
 * Input and placement use the same test-only paths as the rest of SDE's
 * headless harness:
 *   - wlroots virtual pointer/keyboard for pointer and key events,
 *   - serenity-virtual-input for touch,
 *   - serenity-wlcs-control (only when SDE_WLCS_CONTROL=1) for placement.
 */

#include <wlcs/display_server.h>
#include <wlcs/keyboard.h>
#include <wlcs/pointer.h>
#include <wlcs/touch.h>

#include <wayland-client.h>

extern "C" {
#include "serenity-virtual-input-client-protocol.h"
#include "serenity-wlcs-control-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
}

#include <xkbcommon/xkbcommon.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t output_width = 1280;
constexpr uint32_t output_height = 720;

uint32_t now_msec()
{
    struct timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

int connect_socket(std::string const& path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    sockaddr_un address { };
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct SdeServer {
    WlcsDisplayServer api;
    pid_t child { -1 };
    std::string socket_name;
    std::string socket_path;
    std::string runtime_dir;
    bool owns_runtime_dir { false };

    wl_display* control_display { nullptr };
    wl_registry* control_registry { nullptr };
    wl_seat* seat { nullptr };
    zwlr_virtual_pointer_manager_v1* virtual_pointer_manager { nullptr };
    zwp_virtual_keyboard_manager_v1* virtual_keyboard_manager { nullptr };
    serenity_virtual_input_manager_v1* virtual_input_manager { nullptr };

    // Create one device of each kind before the WLCS client connects. That makes
    // the seat capabilities stable (pointer+keyboard+touch) for the whole test;
    // otherwise each per-test virtual device would send a capabilities change,
    // and WLCS's client keeps every wl_pointer/wl_keyboard resource it creates.
    zwlr_virtual_pointer_v1* prime_pointer { nullptr };
    zwp_virtual_keyboard_v1* prime_keyboard { nullptr };
    serenity_virtual_touch_v1* prime_touch { nullptr };

    serenity_wlcs_control_v1* control { nullptr };
};

SdeServer& server_from(WlcsDisplayServer* server)
{
    return *reinterpret_cast<SdeServer*>(server);
}

SdeServer const& server_from(WlcsDisplayServer const* server)
{
    return *reinterpret_cast<SdeServer const*>(server);
}

bool wait_for_socket(SdeServer& server)
{
    // The compositor creates the socket after wlr_backend_start(); on a cold
    // machine the first test can take a moment to load the Lagom libraries.
    for (int attempt = 0; attempt < 600; ++attempt) {
        if (access(server.socket_path.c_str(), F_OK) == 0)
            return true;
        int status = 0;
        if (waitpid(server.child, &status, WNOHANG) == server.child)
            return false;
        usleep(50 * 1000);
    }
    return false;
}

void registry_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    auto& self = *static_cast<SdeServer*>(data);
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        self.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7));
        return;
    }
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        self.virtual_pointer_manager = static_cast<zwlr_virtual_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2));
        return;
    }
    if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        self.virtual_keyboard_manager = static_cast<zwp_virtual_keyboard_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        return;
    }
    if (strcmp(interface, serenity_virtual_input_manager_v1_interface.name) == 0) {
        self.virtual_input_manager = static_cast<serenity_virtual_input_manager_v1*>(
            wl_registry_bind(registry, name, &serenity_virtual_input_manager_v1_interface, 1));
        return;
    }
    if (strcmp(interface, serenity_wlcs_control_v1_interface.name) == 0) {
        self.control = static_cast<serenity_wlcs_control_v1*>(
            wl_registry_bind(registry, name, &serenity_wlcs_control_v1_interface, 1));
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t)
{
}

constexpr wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int create_keymap_fd(char const* keymap, size_t size)
{
    int fd = static_cast<int>(syscall(SYS_memfd_create, "sde-wlcs-keymap", MFD_CLOEXEC));
    if (fd < 0)
        return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memcpy(data, keymap, size);
    munmap(data, size);
    lseek(fd, 0, SEEK_SET);
    return fd;
}

void send_keymap(zwp_virtual_keyboard_v1* keyboard)
{
    if (!keyboard)
        return;

    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    char* keymap_text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
    if (keymap_text) {
        size_t size = strlen(keymap_text) + 1;
        int fd = create_keymap_fd(keymap_text, size);
        if (fd >= 0) {
            zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(size));
            close(fd);
        }
        free(keymap_text);
    }
    if (keymap)
        xkb_keymap_unref(keymap);
    if (context)
        xkb_context_unref(context);
}

void prime_input_devices(SdeServer& self)
{
    if (self.virtual_pointer_manager)
        self.prime_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(self.virtual_pointer_manager, self.seat);
    if (self.virtual_keyboard_manager && self.seat) {
        self.prime_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(self.virtual_keyboard_manager, self.seat);
        send_keymap(self.prime_keyboard);
    }
    if (self.virtual_input_manager)
        self.prime_touch = serenity_virtual_input_manager_v1_create_touch(self.virtual_input_manager);
    wl_display_roundtrip(self.control_display);
}

void start(WlcsDisplayServer* server)
{
    auto& self = server_from(server);
    if (self.child > 0)
        return;

    char const* server_path = getenv("SDE_WINDOW_SERVER");
    std::string path = server_path && *server_path
        ? server_path
        : std::string(getenv("SDE_ROOT") ? getenv("SDE_ROOT") : ".") + "/build/WindowServer/WindowServer";

    self.socket_name = "sde-wlcs-" + std::to_string(getpid()) + "-" + std::to_string(reinterpret_cast<uintptr_t>(&self));
    if (self.socket_name.size() > 100)
        self.socket_name.resize(100);

    auto const* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        char template_path[] = "/tmp/sde-wlcs-runtime-XXXXXX";
        char* made = mkdtemp(template_path);
        if (!made) {
            fprintf(stderr, "sde-wlcs: mkdtemp failed: %s\n", strerror(errno));
            _exit(1);
        }
        self.runtime_dir = made;
        self.owns_runtime_dir = true;
        setenv("XDG_RUNTIME_DIR", self.runtime_dir.c_str(), 1);
    } else {
        self.runtime_dir = runtime;
    }
    self.socket_path = self.runtime_dir + "/" + self.socket_name;

    auto state_dir = self.runtime_dir + "/state";
    auto session_dir = self.runtime_dir + "/session";
    auto wm_portal = self.runtime_dir + "/wm";
    mkdir(state_dir.c_str(), 0700);
    mkdir(session_dir.c_str(), 0700);

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "sde-wlcs: fork failed: %s\n", strerror(errno));
        _exit(1);
    }
    if (child == 0) {
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("DISPLAY");
        unsetenv("SDE_HEARTBEAT_FILE");
        setenv("WLR_BACKENDS", "headless", 1);
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
        setenv("WLR_RENDERER", "pixman", 1);
        setenv("SDE_XWAYLAND", "0", 1);
        setenv("SDE_SESSION_PORTAL", "0", 1);
        setenv("SDE_OUTPUT_SCALE", "1", 1);
        setenv("SDE_STATE_DIR", state_dir.c_str(), 1);
        setenv("SDE_SESSION_DIR", session_dir.c_str(), 1);
        setenv("SDE_WM_PORTAL_PATH", wm_portal.c_str(), 1);
        setenv("SDE_WLCS_CONTROL", "1", 1);
        if (!getenv("SERENITY_RES_ROOT")) {
            auto res = std::string(getenv("SDE_ROOT") ? getenv("SDE_ROOT") : ".") + "/../libgui-wayland/serenity/Base/res";
            setenv("SERENITY_RES_ROOT", res.c_str(), 1);
        }
        execl(path.c_str(), path.c_str(), "-n", self.socket_name.c_str(), static_cast<char*>(nullptr));
        fprintf(stderr, "sde-wlcs: exec '%s' failed: %s\n", path.c_str(), strerror(errno));
        _exit(127);
    }
    self.child = child;
    if (!wait_for_socket(self)) {
        fprintf(stderr, "sde-wlcs: WindowServer did not create '%s'\n", self.socket_path.c_str());
        kill(self.child, SIGKILL);
        waitpid(self.child, nullptr, 0);
        self.child = -1;
        _exit(1);
    }

    int fd = connect_socket(self.socket_path);
    if (fd < 0) {
        fprintf(stderr, "sde-wlcs: control socket connect failed: %s\n", strerror(errno));
        _exit(1);
    }
    self.control_display = wl_display_connect_to_fd(fd);
    if (!self.control_display) {
        close(fd);
        fprintf(stderr, "sde-wlcs: control wl_display_connect_to_fd failed\n");
        _exit(1);
    }
    self.control_registry = wl_display_get_registry(self.control_display);
    wl_registry_add_listener(self.control_registry, &registry_listener, &self);
    if (wl_display_roundtrip(self.control_display) < 0) {
        fprintf(stderr, "sde-wlcs: control registry roundtrip failed\n");
        _exit(1);
    }
    prime_input_devices(self);
}

void stop(WlcsDisplayServer* server)
{
    auto& self = server_from(server);

    if (self.control_display) {
        if (self.prime_touch)
            serenity_virtual_touch_v1_destroy(self.prime_touch);
        if (self.prime_keyboard)
            zwp_virtual_keyboard_v1_destroy(self.prime_keyboard);
        if (self.prime_pointer)
            zwlr_virtual_pointer_v1_destroy(self.prime_pointer);
        self.prime_touch = nullptr;
        self.prime_keyboard = nullptr;
        self.prime_pointer = nullptr;
        if (self.seat)
            wl_seat_destroy(self.seat);
        if (self.virtual_pointer_manager)
            zwlr_virtual_pointer_manager_v1_destroy(self.virtual_pointer_manager);
        if (self.virtual_keyboard_manager)
            zwp_virtual_keyboard_manager_v1_destroy(self.virtual_keyboard_manager);
        if (self.virtual_input_manager)
            serenity_virtual_input_manager_v1_destroy(self.virtual_input_manager);
        if (self.control)
            serenity_wlcs_control_v1_destroy(self.control);
        if (self.control_registry)
            wl_registry_destroy(self.control_registry);
        wl_display_disconnect(self.control_display);
        self.control_display = nullptr;
        self.control_registry = nullptr;
        self.seat = nullptr;
        self.virtual_pointer_manager = nullptr;
        self.virtual_keyboard_manager = nullptr;
        self.virtual_input_manager = nullptr;
        self.control = nullptr;
    }

    if (self.child > 0) {
        kill(self.child, SIGTERM);
        for (int attempt = 0; attempt < 100; ++attempt) {
            int status = 0;
            if (waitpid(self.child, &status, WNOHANG) == self.child) {
                self.child = -1;
                break;
            }
            usleep(50 * 1000);
        }
        if (self.child > 0) {
            kill(self.child, SIGKILL);
            waitpid(self.child, nullptr, 0);
            self.child = -1;
        }
    }
    unlink(self.socket_path.c_str());
    if (self.owns_runtime_dir) {
        rmdir((self.runtime_dir + "/state").c_str());
        rmdir((self.runtime_dir + "/session").c_str());
        rmdir(self.runtime_dir.c_str());
        self.owns_runtime_dir = false;
    }
}

int create_client_socket(WlcsDisplayServer* server)
{
    auto& self = server_from(server);
    return connect_socket(self.socket_path);
}

struct ControlBinding {
    serenity_wlcs_control_v1* control { nullptr };
};

void bind_control_global(void* data, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    auto& binding = *static_cast<ControlBinding*>(data);
    if (strcmp(interface, serenity_wlcs_control_v1_interface.name) == 0)
        binding.control = static_cast<serenity_wlcs_control_v1*>(
            wl_registry_bind(registry, name, &serenity_wlcs_control_v1_interface, version < 1 ? version : 1));
}

void bind_control_global_remove(void*, wl_registry*, uint32_t)
{
}

constexpr wl_registry_listener control_binding_listener = {
    .global = bind_control_global,
    .global_remove = bind_control_global_remove,
};

void position_window_absolute(WlcsDisplayServer*, wl_display* client, wl_surface* surface, int x, int y)
{
    if (!client || !surface)
        return;

    // The request must travel on the connection that owns the surface; a
    // Wayland object from another client cannot be referenced. Binding the
    // test-only global here also means the compositor already knows the
    // client from the request itself.
    ControlBinding binding;
    wl_registry* registry = wl_display_get_registry(client);
    wl_registry_add_listener(registry, &control_binding_listener, &binding);
    if (wl_display_roundtrip(client) < 0) {
        wl_registry_destroy(registry);
        return;
    }
    wl_registry_destroy(registry);
    if (!binding.control)
        return;

    serenity_wlcs_control_v1_move_surface(binding.control, surface, x, y);
    wl_display_flush(client);
    serenity_wlcs_control_v1_destroy(binding.control);
}

struct SdePointer {
    WlcsPointer api;
    SdeServer* server { nullptr };
    zwlr_virtual_pointer_v1* pointer { nullptr };
};

void pointer_move_absolute(WlcsPointer* pointer, wl_fixed_t x, wl_fixed_t y)
{
    auto& self = *reinterpret_cast<SdePointer*>(pointer);
    if (!self.pointer || !self.server->control_display)
        return;

    double px = wl_fixed_to_double(x);
    double py = wl_fixed_to_double(y);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > output_width) px = output_width;
    if (py > output_height) py = output_height;
    zwlr_virtual_pointer_v1_motion_absolute(self.pointer, now_msec(),
        static_cast<uint32_t>(px), static_cast<uint32_t>(py), output_width, output_height);
    zwlr_virtual_pointer_v1_frame(self.pointer);
    wl_display_flush(self.server->control_display);
}

void pointer_move_relative(WlcsPointer* pointer, wl_fixed_t dx, wl_fixed_t dy)
{
    auto& self = *reinterpret_cast<SdePointer*>(pointer);
    if (!self.pointer || !self.server->control_display)
        return;
    zwlr_virtual_pointer_v1_motion(self.pointer, now_msec(), dx, dy);
    zwlr_virtual_pointer_v1_frame(self.pointer);
    wl_display_flush(self.server->control_display);
}

void pointer_button_up(WlcsPointer* pointer, int button)
{
    auto& self = *reinterpret_cast<SdePointer*>(pointer);
    if (!self.pointer || !self.server->control_display)
        return;
    zwlr_virtual_pointer_v1_button(self.pointer, now_msec(), static_cast<uint32_t>(button), WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(self.pointer);
    wl_display_flush(self.server->control_display);
}

void pointer_button_down(WlcsPointer* pointer, int button)
{
    auto& self = *reinterpret_cast<SdePointer*>(pointer);
    if (!self.pointer || !self.server->control_display)
        return;
    zwlr_virtual_pointer_v1_button(self.pointer, now_msec(), static_cast<uint32_t>(button), WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(self.pointer);
    wl_display_flush(self.server->control_display);
}

void pointer_destroy(WlcsPointer* pointer)
{
    auto* self = reinterpret_cast<SdePointer*>(pointer);
    if (self->pointer)
        zwlr_virtual_pointer_v1_destroy(self->pointer);
    delete self;
}

WlcsPointer* create_pointer(WlcsDisplayServer* server)
{
    auto& self = server_from(server);
    auto* pointer = new SdePointer { };
    pointer->server = &self;
    pointer->api = {
        WLCS_POINTER_VERSION,
        pointer_move_absolute,
        pointer_move_relative,
        pointer_button_up,
        pointer_button_down,
        pointer_destroy,
    };
    if (self.virtual_pointer_manager)
        pointer->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(self.virtual_pointer_manager, self.seat);
    return &pointer->api;
}

struct SdeTouch {
    WlcsTouch api;
    SdeServer* server { nullptr };
    serenity_virtual_touch_v1* touch { nullptr };
};

void touch_down(WlcsTouch* touch, wl_fixed_t x, wl_fixed_t y)
{
    auto& self = *reinterpret_cast<SdeTouch*>(touch);
    if (!self.touch || !self.server->control_display)
        return;
    serenity_virtual_touch_v1_down(self.touch, now_msec(), 0, x, y);
    wl_display_flush(self.server->control_display);
}

void touch_move(WlcsTouch* touch, wl_fixed_t x, wl_fixed_t y)
{
    auto& self = *reinterpret_cast<SdeTouch*>(touch);
    if (!self.touch || !self.server->control_display)
        return;
    serenity_virtual_touch_v1_motion(self.touch, now_msec(), 0, x, y);
    wl_display_flush(self.server->control_display);
}

void touch_up(WlcsTouch* touch)
{
    auto& self = *reinterpret_cast<SdeTouch*>(touch);
    if (!self.touch || !self.server->control_display)
        return;
    serenity_virtual_touch_v1_up(self.touch, now_msec(), 0);
    wl_display_flush(self.server->control_display);
}

void touch_destroy(WlcsTouch* touch)
{
    auto* self = reinterpret_cast<SdeTouch*>(touch);
    if (self->touch)
        serenity_virtual_touch_v1_destroy(self->touch);
    delete self;
}

WlcsTouch* create_touch(WlcsDisplayServer* server)
{
    auto& self = server_from(server);
    auto* touch = new SdeTouch { };
    touch->server = &self;
    touch->api = {
        WLCS_TOUCH_VERSION,
        touch_down,
        touch_move,
        touch_up,
        touch_destroy,
    };
    if (self.virtual_input_manager)
        touch->touch = serenity_virtual_input_manager_v1_create_touch(self.virtual_input_manager);
    return &touch->api;
}

struct SdeKeyboard {
    WlcsKeyboard api;
    SdeServer* server { nullptr };
    zwp_virtual_keyboard_v1* keyboard { nullptr };
};

void keyboard_key_down(WlcsKeyboard* keyboard, int scancode)
{
    auto& self = *reinterpret_cast<SdeKeyboard*>(keyboard);
    if (!self.keyboard || !self.server->control_display)
        return;
    zwp_virtual_keyboard_v1_key(self.keyboard, now_msec(), static_cast<uint32_t>(scancode), WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_display_flush(self.server->control_display);
}

void keyboard_key_up(WlcsKeyboard* keyboard, int scancode)
{
    auto& self = *reinterpret_cast<SdeKeyboard*>(keyboard);
    if (!self.keyboard || !self.server->control_display)
        return;
    zwp_virtual_keyboard_v1_key(self.keyboard, now_msec(), static_cast<uint32_t>(scancode), WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(self.server->control_display);
}

void keyboard_destroy(WlcsKeyboard* keyboard)
{
    auto* self = reinterpret_cast<SdeKeyboard*>(keyboard);
    if (self->keyboard)
        zwp_virtual_keyboard_v1_destroy(self->keyboard);
    delete self;
}

WlcsKeyboard* create_keyboard(WlcsDisplayServer* server)
{
    auto& self = server_from(server);
    auto* keyboard = new SdeKeyboard { };
    keyboard->server = &self;
    keyboard->api = {
        WLCS_KEYBOARD_VERSION,
        keyboard_key_down,
        keyboard_key_up,
        keyboard_destroy,
    };
    if (!self.virtual_keyboard_manager || !self.seat)
        return &keyboard->api;

    keyboard->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(self.virtual_keyboard_manager, self.seat);
    if (!keyboard->keyboard)
        return &keyboard->api;

    send_keymap(keyboard->keyboard);
    wl_display_flush(self.control_display);
    return &keyboard->api;
}

WlcsExtensionDescriptor const extensions[] = {
    { "wl_compositor", 5 },
    { "wl_subcompositor", 1 },
    { "wl_shm", 1 },
    { "wl_output", 4 },
    { "xdg_wm_base", 3 },
    { "wl_seat", 7 },
};

WlcsIntegrationDescriptor const descriptor {
    WLCS_INTEGRATION_DESCRIPTOR_VERSION,
    sizeof(extensions) / sizeof(extensions[0]),
    extensions,
};

WlcsIntegrationDescriptor const* get_descriptor(WlcsDisplayServer const*)
{
    return &descriptor;
}

WlcsDisplayServer* create_server(int, char const**)
{
    auto* server = new SdeServer { };
    server->api.version = WLCS_DISPLAY_SERVER_VERSION;
    server->api.start = start;
    server->api.stop = stop;
    server->api.create_client_socket = create_client_socket;
    server->api.position_window_absolute = position_window_absolute;
    server->api.create_pointer = create_pointer;
    server->api.create_touch = create_touch;
    server->api.get_descriptor = get_descriptor;
    server->api.start_on_this_thread = nullptr;
    server->api.create_keyboard = create_keyboard;
    return &server->api;
}

void destroy_server(WlcsDisplayServer* server)
{
    stop(server);
    delete reinterpret_cast<SdeServer*>(server);
}

} // namespace

extern "C" __attribute__((visibility("default"))) WlcsServerIntegration const wlcs_server_integration {
    WLCS_SERVER_INTEGRATION_VERSION,
    create_server,
    destroy_server,
};
