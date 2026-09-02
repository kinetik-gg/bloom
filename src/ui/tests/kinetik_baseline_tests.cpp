#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QSplitter>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

EditorRegistry makeRegistry() {
    EditorRegistry registry;
    (void)registry.registerEditor(
        {"bloom.probe", "Probe", [](QWidget* parent) -> QWidget* { return new QWidget(parent); }});
    return registry;
}

void testPanelsAreSeparatedByARealGutter(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToSingleArea("bloom.probe");
    auto* area = host.activeArea();
    expectations.expect(area != nullptr, "the workspace has an area to split");
    if (area == nullptr) {
        return;
    }
    (void)host.splitArea(*area, Qt::Horizontal, "bloom.probe", 0.5);

    const auto splitters = host.findChildren<QSplitter*>(QStringLiteral("workspaceSplitter"));
    expectations.expect(!splitters.isEmpty(), "splitting produces a workspace splitter");
    for (const auto* splitter : splitters) {
        // The gutter is a visible Background gap between panels, not a hairline seam: the handle
        // width is what makes that gap real and grabbable.
        expectations.expect(splitter->handleWidth() == kit::px(kit::Spacing::Gutter),
                            "every workspace splitter's handle is the gutter token wide");
    }
    expectations.expect(kit::px(kit::Spacing::Gutter) == 6, "and the gutter token is 6");
}

void testTheSplitterHandleIsPaintedInBackground(Expectations& expectations) {
    // The gap has to read as the window behind the panels rather than as a light divider, so the
    // handle takes Background -- asserted through the generated sheet rather than a screenshot.
    const QString sheet = kit::kinetikStyleSheet();
    const qsizetype rule = sheet.indexOf(QStringLiteral("QSplitter::handle"));
    expectations.expect(rule >= 0, "the theme styles the splitter handle");
    if (rule < 0) {
        return;
    }
    const QString block = sheet.mid(rule, 80);
    expectations.expect(
        block.contains(kit::hex(kit::Color::Background)),
        "the handle is painted in Background, so the gutter reads as the gap it is");
}

void testTheNodeGraphSitsOnTheKinetikBackground(Expectations& expectations) {
    NodeGraphicsScene scene;
    expectations.expect(scene.backgroundBrush().color() == kit::color(kit::Color::Background),
                        "the node graph's canvas is the Kinetik background, not its own grey");
    expectations.expect(scene.objectName() == QStringLiteral("nodeGraphicsScene"),
                        "and the scene keeps its objectName");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testPanelsAreSeparatedByARealGutter(expectations);
    testTheSplitterHandleIsPaintedInBackground(expectations);
    testTheNodeGraphSitsOnTheKinetikBackground(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
