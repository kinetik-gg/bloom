#include "window_fixture.hpp"
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <iostream>

namespace {
int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("Metrics");
    bloom::ui::kit::installKinetikTheme(app);
    using namespace bloom::ui;
    test::WindowFixture fixture;
    int failures = 0, controls = 0, icons = 0;
    const auto expect = [&](bool condition, const QWidget* widget, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << widget->metaObject()->className() << ' '
                      << widget->objectName().toStdString() << " height=" << widget->height()
                      << ": " << message << '\n';
        }
    };
    const auto panels = fixture.window->findChildren<EditorArea*>();
    expect(panels.size() == 5, fixture.window.get(), "fixture must exercise all five panels");
    for (auto* panel : panels) {
        auto* header = panel->findChild<QWidget*>("editorHeader");
        expect(header && header->height() == kit::px(kit::Size::HeaderRow), panel, "header token");
        if (auto* footer = panel->findChild<QWidget*>("editorFooter"))
            expect(footer->height() == kit::px(kit::Size::FooterRow), footer, "footer token");
        for (auto* widget : panel->findChildren<QWidget*>()) {
            if (widget->property("chromeControl").toBool() ||
                widget->objectName() == "editorTypePicker" ||
                widget->objectName() == "maximizeAreaButton") {
                expect(widget->height() == kit::px(kit::Size::Control), widget,
                       "chrome child must use Control");
                ++controls;
            }
            if (qobject_cast<kit::KDropdown*>(widget) || qobject_cast<kit::KIconButton*>(widget) ||
                qobject_cast<kit::KMenuButton*>(widget) ||
                qobject_cast<kit::KValueField*>(widget) || qobject_cast<kit::KSwitch*>(widget) ||
                qobject_cast<kit::KSlider*>(widget) || qobject_cast<kit::KSearchField*>(widget) ||
                qobject_cast<kit::KLabel*>(widget) || qobject_cast<kit::KColorChip*>(widget)) {
                expect(widget->height() == kit::px(kit::Size::Control), widget,
                       "kit control token");
                ++controls;
            }
            if (const auto* button = qobject_cast<kit::KButton*>(widget)) {
                const auto expected = button->controlSize() == kit::KButton::ControlSize::Roomy
                                          ? kit::Size::ControlRoomy
                                          : kit::Size::Control;
                expect(button->height() == kit::px(expected), button, "button role token");
                ++controls;
            }
            if (qobject_cast<kit::KRow*>(widget)) {
                expect(widget->height() == kit::px(kit::Size::ListRow), widget, "list row token");
                if (widget->isVisible()) {
                    for (auto* cell :
                         widget->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
                        const auto role = cell->property("rowCell").toString();
                        if (role.isEmpty())
                            continue;
                        expect(widget->rect().contains(cell->geometry()), cell,
                               "row cell must fit inside its row");
                        expect(cell->width() == kit::px(role == "toggle"
                                                            ? kit::Size::ToggleCell
                                                            : kit::Size::DropdownWidth),
                               cell, "row column pitch");
                    }
                }
            }
            if (qobject_cast<kit::KPropertyRow*>(widget))
                expect(widget->height() == kit::px(kit::Size::PropertyRow), widget,
                       "property row token");
            if (auto* button = qobject_cast<QToolButton*>(widget);
                button && !button->icon().isNull()) {
                const auto pixmap =
                    button->icon().pixmap(button->iconSize(), widget->devicePixelRatioF());
                const auto expected =
                    QSize(qRound(button->iconSize().width() * widget->devicePixelRatioF()),
                          qRound(button->iconSize().height() * widget->devicePixelRatioF()));
                expect(pixmap.size() == expected &&
                           pixmap.devicePixelRatio() == widget->devicePixelRatioF(),
                       widget, "icon must be rasterized at process DPR");
                ++icons;
            }
            if (const auto* dropdown = qobject_cast<kit::KDropdown*>(widget)) {
                for (int index = 0; index < dropdown->count(); ++index) {
                    const auto icon = dropdown->itemIcon(index);
                    if (icon.isNull())
                        continue;
                    const auto box = kit::px(kit::Size::IconChrome);
                    const auto pixmap = icon.pixmap(QSize(box, box), widget->devicePixelRatioF());
                    expect(pixmap.width() == qRound(box * widget->devicePixelRatioF()) &&
                               pixmap.devicePixelRatio() == widget->devicePixelRatioF(),
                           widget, "dropdown icon variant must use exact device pixels");
                    ++icons;
                }
            }
            if (const auto* toggle = qobject_cast<kit::KIconToggle*>(widget)) {
                const auto pixmap = toggle->glyphPixmap();
                expect(pixmap.width() ==
                           qRound(kit::px(kit::Size::IconControl) * widget->devicePixelRatioF()),
                       widget, "toggle integer physical glyph box");
                ++icons;
            }
        }
    }
    expect(controls > 30 && icons > 10, fixture.window.get(),
           "audit must inspect real controls and glyphs");
    std::cout << "Audited " << panels.size() << " panels, " << controls << " controls, " << icons
              << " icons at DPR " << fixture.window->devicePixelRatioF() << '\n';
    return failures == 0 ? 0 : 1;
}

} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
