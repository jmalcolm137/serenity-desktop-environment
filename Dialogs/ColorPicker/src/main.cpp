/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ColorPicker — the SDE color picker for the freedesktop Screenshot portal's
// PickColor request (R5). The xdg-desktop-portal-serenity backend freezes the
// screen into a PNG, launches this LibGUI app with that image, the user clicks a
// pixel, and the app prints "<x> <y>" (image coordinates) for the backend to
// sample. It is a plain SerenityOS LibGUI app.

#include <AK/Function.h>
#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>

class PickerWidget final : public GUI::ImageWidget {
    C_OBJECT(PickerWidget);

public:
    Function<void(Gfx::IntPoint)> on_pick;

private:
    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() == GUI::MouseButton::Primary && on_pick)
            on_pick(event.position());
        GUI::ImageWidget::mousedown_event(event);
    }
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView image_path;
    Core::ArgsParser args_parser;
    args_parser.add_option(image_path, "Frozen screenshot to pick a color from", "image", 'i', "path");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title("Pick a Color");
    window->set_resizable(false);

    auto widget_ptr = window->set_main_widget<PickerWidget>();
    auto& widget = *widget_ptr;
    widget.set_should_stretch(true);
    if (!image_path.is_empty())
        widget.load_from_file(image_path);
    if (auto const* bitmap = widget.bitmap())
        window->resize(bitmap->size());
    else
        window->resize(400, 300);

    widget.on_pick = [&](Gfx::IntPoint position) {
        outln("{} {}", position.x(), position.y());
        app->quit();
    };

    window->show();
    return app->exec();
}
