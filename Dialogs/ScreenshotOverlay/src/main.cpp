/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ScreenshotOverlay — the SDE area-selection overlay for the freedesktop
// Screenshot portal's interactive mode (R5). The xdg-desktop-portal-serenity
// backend freezes the screen into a PNG and launches this LibGUI app with it;
// the user drags a rectangle and the selected area is printed to stdout as
// "<x> <y> <width> <height>" (exit 0). A click without a drag cancels (exit 1).

#include <AK/Function.h>
#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>
#include <LibMain/Main.h>

class SelectionWidget final : public GUI::ImageWidget {
    C_OBJECT(SelectionWidget);

public:
    Function<void(Gfx::IntRect)> on_selection;

private:
    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (event.button() != GUI::MouseButton::Primary)
            return;
        m_origin = event.position();
        m_current = m_origin;
        m_dragging = true;
        update();
    }

    virtual void mousemove_event(GUI::MouseEvent& event) override
    {
        if (!m_dragging)
            return;
        m_current = event.position();
        update();
    }

    virtual void mouseup_event(GUI::MouseEvent& event) override
    {
        if (!m_dragging)
            return;
        m_dragging = false;
        m_current = event.position();
        update();
        if (on_selection)
            on_selection(selection());
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::ImageWidget::paint_event(event);
        if (!m_dragging && m_origin == m_current)
            return;
        GUI::Painter painter(*this);
        painter.draw_rect(selection(), Gfx::Color::Red);
    }

    Gfx::IntRect selection() const
    {
        auto rect = Gfx::IntRect::from_two_points(m_origin, m_current);
        if (auto const* bitmap = this->bitmap()) {
            Gfx::IntRect bounds { {}, bitmap->size() };
            return rect.intersected(bounds);
        }
        return rect;
    }

    Gfx::IntPoint m_origin;
    Gfx::IntPoint m_current;
    bool m_dragging { false };
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView image_path;
    Core::ArgsParser args_parser;
    args_parser.add_option(image_path, "Frozen screenshot to select from", "image", 'i', "path");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title("Select an Area");
    window->set_resizable(false);

    auto widget_ptr = window->set_main_widget<SelectionWidget>();
    auto& widget = *widget_ptr;
    widget.set_should_stretch(true);
    if (!image_path.is_empty())
        widget.load_from_file(image_path);
    if (auto const* bitmap = widget.bitmap())
        window->resize(bitmap->size());
    else
        window->resize(400, 300);

    int result = 1;
    widget.on_selection = [&](Gfx::IntRect rect) {
        if (rect.width() > 0 && rect.height() > 0) {
            outln("{} {} {} {}", rect.x(), rect.y(), rect.width(), rect.height());
            result = 0;
        }
        app->quit();
    };

    window->show();
    app->exec();
    return result;
}
