// Workspace presets (docs/architecture/workspace-layout.md, "Workspace presets"): each built-in
// arrangement, panel preservation, user presets stored in application preferences, the "(edited)"
// label, and the Export editor's reveal policy.

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/workspace_host.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <QApplication>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QWidget>

#include <iostream>
#include <map>
#include <source_location>
#include <string>

namespace {

using namespace bloom::ui;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

std::map<std::string, int> constructed;

EditorRegistry makeRegistry() {
    EditorRegistry registry;
    for (const auto* id : {"bloom.assets", "bloom.viewer", "bloom.nodes", "bloom.properties",
                           "bloom.timeline", "bloom.export", "bloom.probe"}) {
        (void)registry.registerEditor({id, QString::fromLatin1(id), [id](QWidget* parent) {
                                           ++constructed[id];
                                           return new QWidget(parent);
                                       }});
    }
    return registry;
}

EditorArea* areaFor(WorkspaceHost& host, const std::string& id) {
    for (auto* area : host.findChildren<EditorArea*>())
        if (area->editorId() == id)
            return area;
    return nullptr;
}

QSplitter* rootSplitter(WorkspaceHost& host) {
    for (auto* splitter : host.findChildren<QSplitter*>())
        if (splitter->parentWidget() == &host)
            return splitter;
    return nullptr;
}

void show(WorkspaceHost& host) {
    host.resize(1600, 900);
    host.show();
    QCoreApplication::processEvents();
}

void testBuiltInArrangements(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    show(host);
    WorkspacePresets presets(host);
    expectations.expect(presets.builtInPresets().size() >= 3 &&
                            presets.builtInPresets()[0].name == QStringLiteral("Default") &&
                            presets.builtInPresets()[1].name == QStringLiteral("Node") &&
                            presets.builtInPresets()[2].name == QStringLiteral("Export"),
                        "the built-in presets are Default, Node and Export, in that order");

    expectations.expect(presets.apply(QStringLiteral("Default")), "Default applies");
    auto* root = rootSplitter(host);
    expectations.expect(host.areaCount() == 4 && areaFor(host, "bloom.nodes") == nullptr &&
                            root != nullptr && root->orientation() == Qt::Horizontal &&
                            root->count() == 2 && areaFor(host, "bloom.properties") != nullptr &&
                            root->widget(1) == areaFor(host, "bloom.properties"),
                        "Default: Properties on the right, the rest to its left");
    auto* left = root != nullptr ? qobject_cast<QSplitter*>(root->widget(0)) : nullptr;
    auto* top = left != nullptr ? qobject_cast<QSplitter*>(left->widget(0)) : nullptr;
    expectations.expect(left != nullptr && left->orientation() == Qt::Vertical && top != nullptr &&
                            top->orientation() == Qt::Horizontal &&
                            top->widget(0) == areaFor(host, "bloom.assets") &&
                            top->widget(1) == areaFor(host, "bloom.viewer") &&
                            left->widget(1) == areaFor(host, "bloom.timeline"),
                        "Default: Assets | Viewer over the Timeline, which spans both");

    expectations.expect(presets.apply(QStringLiteral("Node")), "Node applies");
    root = rootSplitter(host);
    top = root != nullptr ? qobject_cast<QSplitter*>(root->widget(0)) : nullptr;
    expectations.expect(host.areaCount() == 3 && root != nullptr &&
                            root->orientation() == Qt::Vertical && top != nullptr &&
                            top->widget(0) == areaFor(host, "bloom.viewer") &&
                            top->widget(1) == areaFor(host, "bloom.nodes") &&
                            root->widget(1) == areaFor(host, "bloom.timeline"),
                        "Node: Viewer | Nodes over the Timeline");

    expectations.expect(presets.apply(QStringLiteral("Export")), "Export applies");
    root = rootSplitter(host);
    expectations.expect(host.areaCount() == 2 && root != nullptr &&
                            root->orientation() == Qt::Horizontal &&
                            root->widget(0) == areaFor(host, "bloom.assets") &&
                            root->widget(1) == areaFor(host, "bloom.export"),
                        "Export: Assets left, Export right");
    expectations.expect(!presets.apply(QStringLiteral("No such")), "an unknown preset is refused");
    host.hide();
}

void testPanelsThePresetKeepsAreNotRebuilt(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    show(host);
    WorkspacePresets presets(host);
    auto* viewer = areaFor(host, "bloom.viewer");
    auto* timeline = areaFor(host, "bloom.timeline");
    const auto viewersBefore = constructed["bloom.viewer"];
    const auto timelinesBefore = constructed["bloom.timeline"];
    for (const auto* name : {"Node", "Default", "Animation", "Node"})
        expectations.expect(presets.apply(QString::fromLatin1(name)), "a preset applies");
    expectations.expect(areaFor(host, "bloom.viewer") == viewer &&
                            areaFor(host, "bloom.timeline") == timeline &&
                            constructed["bloom.viewer"] == viewersBefore &&
                            constructed["bloom.timeline"] == timelinesBefore,
                        "the Viewer and Timeline survive every preset change: same panel, same "
                        "editor, nothing re-created");
    host.hide();
}

void testUserPresetsSaveUpdateDelete(Expectations& expectations) {
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    show(host);
    {
        WorkspacePresets presets(host);
        expectations.expect(presets.validateName(QString{}).has_value() &&
                                presets.validateName(QStringLiteral("node")).has_value() &&
                                !presets.validateName(QStringLiteral("Mine")).has_value(),
                            "a name must be new and not a built-in's");
        expectations.expect(presets.apply(QStringLiteral("Node")), "Node applies");
        expectations.expect(presets.saveCurrentAs(QStringLiteral("Mine")) &&
                                presets.activeName() == QStringLiteral("Mine") &&
                                presets.activeIsUser() && !presets.isEdited() &&
                                !presets.saveCurrentAs(QStringLiteral("mine")),
                            "Save current layout as stores a user preset and makes it active");
        // A manual change: swap the Nodes panel for the probe editor.
        expectations.expect(areaFor(host, "bloom.nodes")->setEditorId("bloom.probe") &&
                                presets.isEdited() &&
                                presets.label() == QStringLiteral("Mine (edited)"),
                            "changing the layout by hand shows \"(edited)\"");
        expectations.expect(presets.updateActive() && !presets.isEdited() &&
                                presets.label() == QStringLiteral("Mine"),
                            "Update replaces the preset with the live layout");
        expectations.expect(
            presets.apply(QStringLiteral("Default")) && presets.apply(QStringLiteral("Mine")) &&
                areaFor(host, "bloom.probe") != nullptr && areaFor(host, "bloom.nodes") == nullptr,
            "the updated preset applies as saved");
    }
    {
        WorkspacePresets reloaded(host);
        expectations.expect(reloaded.userPresets().size() == 1 &&
                                reloaded.userPresets()[0].name == QStringLiteral("Mine") &&
                                reloaded.activeName() == QStringLiteral("Mine"),
                            "user presets and the active one persist in application preferences");
        expectations.expect(reloaded.removeUser(QStringLiteral("Mine")) &&
                                reloaded.userPresets().empty() && reloaded.label() == "Custom" &&
                                host.areaCount() == 3,
                            "Delete removes the preset and keeps the panels; the field reads "
                            "\"Custom\"");
    }
    host.hide();
}

void testDraggingADividerMarksThePresetEdited(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    show(host);
    WorkspacePresets presets(host);
    (void)presets.apply(QStringLiteral("Node"));
    expectations.expect(!presets.isEdited() && presets.label() == QStringLiteral("Node"),
                        "right after choosing a preset it is not edited");
    auto* root = rootSplitter(host);
    root->setSizes({200, 700});
    Q_EMIT root->splitterMoved(200, 1);
    expectations.expect(presets.isEdited() && presets.label() == QStringLiteral("Node (edited)"),
                        "moving a divider far from the preset marks it edited");
    (void)host.splitActiveArea(Qt::Horizontal);
    expectations.expect(presets.isEdited(), "splitting a panel keeps it edited");
    host.hide();
}

void testExportRevealPolicy(Expectations& expectations) {
    const auto registry = makeRegistry();
    {
        // Rule 1: an Export editor already open is activated, never duplicated.
        WorkspaceHost host(registry);
        host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.export",
                                  "bloom.properties");
        show(host);
        const auto count = host.areaCount();
        auto* exportArea = areaFor(host, "bloom.export");
        auto* revealed = host.revealEditor("bloom.export");
        expectations.expect(revealed == exportArea && host.areaCount() == count &&
                                host.activeArea() == exportArea,
                            "an open Export panel is made active and nothing is spawned");
        host.hide();
    }
    {
        // Rule 2: no Export, a Timeline: the Timeline's panel switches to Export in place.
        WorkspaceHost host(registry);
        host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                                  "bloom.properties");
        show(host);
        const auto count = host.areaCount();
        auto* timeline = areaFor(host, "bloom.timeline");
        auto* revealed = host.revealEditor("bloom.export");
        expectations.expect(revealed == timeline && timeline->editorId() == "bloom.export" &&
                                host.areaCount() == count && host.activeArea() == timeline &&
                                areaFor(host, "bloom.timeline") == nullptr,
                            "with no Export panel the Timeline panel becomes Export in place");
        host.hide();
    }
    {
        // Rule 3: neither: a new panel along the bottom.
        WorkspaceHost host(registry);
        host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.probe",
                                  "bloom.properties");
        show(host);
        const auto count = host.areaCount();
        auto* revealed = host.revealEditor("bloom.export");
        auto* root = rootSplitter(host);
        expectations.expect(revealed != nullptr && revealed->editorId() == "bloom.export" &&
                                host.areaCount() == count + 1 && host.activeArea() == revealed &&
                                root != nullptr && root->orientation() == Qt::Vertical &&
                                root->widget(root->count() - 1) == revealed,
                            "with neither, a new Export panel spans the bottom of the workspace");
        host.hide();
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    bloom::ui::kit::installKinetikTheme(application);
    Expectations expectations;
    testBuiltInArrangements(expectations);
    testPanelsThePresetKeepsAreNotRebuilt(expectations);
    testUserPresetsSaveUpdateDelete(expectations);
    testDraggingADividerMarksThePresetEdited(expectations);
    testExportRevealPolicy(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
