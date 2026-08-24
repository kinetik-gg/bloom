#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using bloom::ui::EditorArea;
using bloom::ui::EditorRegistry;
using bloom::ui::WorkspaceHost;
using bloom::ui::WorkspaceLayoutRestoreResult;

bool require(bool condition, int code) {
    if (!condition) {
        std::fprintf(stderr, "Bloom UI smoke assertion failed with code %d\n", code);
    }
    return condition;
}

std::vector<std::string> editorIds(const WorkspaceHost& host) {
    std::vector<std::string> ids;
    const auto areas = host.findChildren<EditorArea*>();
    ids.reserve(static_cast<std::size_t>(areas.size()));
    for (const auto* area : areas) {
        ids.push_back(area->editorId());
    }
    std::ranges::sort(ids);
    return ids;
}

std::vector<std::string> areaIds(const WorkspaceHost& host) {
    std::vector<std::string> ids;
    const auto areas = host.findChildren<EditorArea*>();
    ids.reserve(static_cast<std::size_t>(areas.size()));
    for (const auto* area : areas) {
        ids.push_back(area->areaId().toStdString());
    }
    std::ranges::sort(ids);
    return ids;
}

bool isBinaryLayoutNode(const QJsonObject& node) {
    const auto type = node.value("type").toString();
    if (type == "area") {
        return !node.value("editor").toString().isEmpty();
    }
    if (type != "split") {
        return false;
    }

    const auto children = node.value("children").toArray();
    const auto weights = node.value("weights").toArray();
    return children.size() == 2 && weights.size() == 2 && children.at(0).isObject() &&
           children.at(1).isObject() && isBinaryLayoutNode(children.at(0).toObject()) &&
           isBinaryLayoutNode(children.at(1).toObject());
}

int testRegistry(EditorRegistry& registry) {
    const auto addTestEditor = [&registry](std::string id, QString name) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = [](QWidget* parent) {
                 auto* label = new QLabel("Workspace test editor", parent);
                 label->setObjectName("editorPlaceholder");
                 return label;
             }});
    };
    const bool registered =
        addTestEditor("bloom.viewer", "Compositor") && addTestEditor("bloom.nodes", "Nodes") &&
        addTestEditor("bloom.timeline", "Timeline") && addTestEditor("bloom.media", "Media") &&
        addTestEditor("bloom.properties", "Properties");
    if (!require(registered, 1)) {
        return 1;
    }
    if (!require(registry.editors().size() == 5, 2)) {
        return 2;
    }
    if (!require(!registry.registerEditor(registry.editors().front()), 3)) {
        return 3;
    }
    if (!require(registry.editors().front().id == "bloom.viewer", 4)) {
        return 4;
    }
    return 0;
}

int testSplitCloseAndActivation(const EditorRegistry& registry) {
    WorkspaceHost host(registry);
    host.resize(1000, 700);
    host.show();
    QApplication::processEvents();

    auto* viewer = host.activeArea();
    if (!require(viewer != nullptr && viewer->editorId() == "bloom.viewer", 10)) {
        return 10;
    }
    if (!require(viewer->setEditorId("bloom.properties") &&
                     viewer->editorId() == "bloom.properties",
                 11)) {
        return 11;
    }

    auto* nodes = host.splitArea(*viewer, Qt::Horizontal, "bloom.nodes", 0.4);
    auto* timeline = host.splitArea(*nodes, Qt::Vertical, "bloom.timeline", 0.3);
    if (!require(nodes != nullptr && timeline != nullptr && host.areaCount() == 3, 12)) {
        return 12;
    }
    const auto splitAreaIds = areaIds(host);
    if (!require(std::ranges::adjacent_find(splitAreaIds) == splitAreaIds.end(), 57)) {
        return 57;
    }

    QFocusEvent focusEvent(QEvent::FocusIn, Qt::OtherFocusReason);
    QApplication::sendEvent(viewer, &focusEvent);
    if (!require(host.activeArea() == viewer && viewer->isAreaActive(), 13)) {
        return 13;
    }

    const auto areas = host.findChildren<EditorArea*>();
    for (const auto* area : areas) {
        const auto* picker = area->findChild<QComboBox*>("editorTypePicker");
        if (!require(picker != nullptr && picker->count() == 5, 14)) {
            return 14;
        }
    }
    if (!require(host.findChildren<QLabel*>("editorPlaceholder").size() == host.areaCount(), 15)) {
        return 15;
    }

    host.setActiveArea(timeline);
    auto* liveEditor = viewer->findChild<QLabel*>("editorPlaceholder");
    if (!require(liveEditor != nullptr, 58)) {
        return 58;
    }
    auto* dynamicComposite = new QWidget();
    auto* dynamicChild = new QWidget(dynamicComposite);
    dynamicComposite->setParent(liveEditor);
    QFocusEvent dynamicFocusEvent(QEvent::FocusIn, Qt::OtherFocusReason);
    QApplication::sendEvent(dynamicChild, &dynamicFocusEvent);
    if (!require(host.activeArea() == viewer, 59)) {
        return 59;
    }

    host.setActiveArea(timeline);
    if (!require(host.closeActiveArea() && host.areaCount() == 2, 16)) {
        return 16;
    }
    if (!require(host.closeActiveArea() && host.areaCount() == 1, 17)) {
        return 17;
    }
    if (!require(!host.closeActiveArea() && host.areaCount() == 1, 18)) {
        return 18;
    }
    return 0;
}

int testMaximizeAndPersistence(const EditorRegistry& registry) {
    WorkspaceHost source(registry);
    source.resize(1200, 800);
    auto* viewer = source.activeArea();
    auto* timeline = source.splitArea(*viewer, Qt::Vertical, "bloom.timeline", 0.25);
    auto* nodes = source.splitArea(*viewer, Qt::Horizontal, "bloom.nodes", 0.40);
    if (!require(timeline != nullptr && nodes != nullptr && source.areaCount() == 3, 20)) {
        return 20;
    }
    source.show();
    QApplication::processEvents();
    source.setActiveArea(nodes);

    const QByteArray beforeMaximize = source.saveLayoutState();
    const auto serialized = QJsonDocument::fromJson(beforeMaximize).object();
    if (!require(serialized.value("format").toString() == "bloom.workspace-layout" &&
                     serialized.value("schema").toInt() == 1 &&
                     isBinaryLayoutNode(serialized.value("root").toObject()),
                 19)) {
        return 19;
    }
    source.toggleMaximizeActiveArea();
    if (!require(source.isAreaMaximized() && source.areaCount() == 3, 21)) {
        return 21;
    }

    int invisibleAreas = 0;
    for (const auto* area : source.findChildren<EditorArea*>()) {
        invisibleAreas += area->isVisible() ? 0 : 1;
    }
    if (!require(invisibleAreas == 2, 22)) {
        return 22;
    }
    if (!require(source.saveLayoutState() == beforeMaximize, 52)) {
        return 52;
    }
    if (!require(source.splitActiveArea(Qt::Horizontal) == nullptr && !source.closeActiveArea() &&
                     source.areaCount() == 3,
                 53)) {
        return 53;
    }

    source.toggleMaximizeActiveArea();
    if (!require(!source.isAreaMaximized() && source.areaCount() == 3, 23)) {
        return 23;
    }
    for (const auto* area : source.findChildren<EditorArea*>()) {
        if (!require(!area->isHidden(), 24)) {
            return 24;
        }
    }

    QTemporaryDir settingsDirectory;
    if (!require(settingsDirectory.isValid(), 25)) {
        return 25;
    }
    const auto settingsPath = QDir(settingsDirectory.path()).filePath("workspace.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        source.persistLayout(settings, "test/layout");
        settings.sync();
    }

    WorkspaceHost restored(registry);
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        const auto result = restored.restorePersistedLayout(settings, "test/layout");
        if (!require(result == WorkspaceLayoutRestoreResult::Restored, 26)) {
            return 26;
        }
    }
    if (!require(restored.areaCount() == 3 && editorIds(restored) == editorIds(source) &&
                     areaIds(restored) == areaIds(source),
                 27)) {
        return 27;
    }
    if (!require(restored.activeArea() != nullptr &&
                     restored.activeArea()->editorId() == "bloom.nodes",
                 28)) {
        return 28;
    }
    restored.resize(1200, 800);
    restored.show();
    QApplication::processEvents();

    const auto restoredRoot =
        QJsonDocument::fromJson(restored.saveLayoutState()).object().value("root").toObject();
    const auto restoredRootWeights = restoredRoot.value("weights").toArray();
    const auto restoredTop = restoredRoot.value("children").toArray().at(0).toObject();
    const auto restoredTopWeights = restoredTop.value("weights").toArray();
    if (!require(restoredRoot.value("orientation").toString() == "vertical" &&
                     restoredTop.value("orientation").toString() == "horizontal" &&
                     restoredRootWeights.at(0).toDouble() > restoredRootWeights.at(1).toDouble() &&
                     restoredTopWeights.at(0).toDouble() > restoredTopWeights.at(1).toDouble(),
                 56)) {
        return 56;
    }

    const auto unavailableAreaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::string unavailableEditorId = "pipeline.missing-editor";
    const QJsonObject unavailableRoot = {
        {"type", "area"},
        {"id", unavailableAreaId},
        {"editor", QString::fromStdString(unavailableEditorId)},
        {"active", true},
    };
    const QByteArray unavailableEditorState =
        QJsonDocument(QJsonObject{{"format", "bloom.workspace-layout"},
                                  {"schema", 1},
                                  {"root", unavailableRoot}})
            .toJson(QJsonDocument::Compact);
    if (!require(restored.restoreLayoutState(unavailableEditorState) ==
                         WorkspaceLayoutRestoreResult::Restored &&
                     restored.areaCount() == 1 &&
                     restored.activeArea()->areaId() == unavailableAreaId &&
                     restored.activeArea()->editorId() == unavailableEditorId,
                 29)) {
        return 29;
    }
    const auto* unavailablePlaceholder =
        restored.activeArea()->findChild<QLabel*>("unavailableEditorPlaceholder");
    if (!require(unavailablePlaceholder != nullptr &&
                     unavailablePlaceholder->text().contains(
                         QString::fromStdString(unavailableEditorId)) &&
                     QJsonDocument::fromJson(restored.saveLayoutState())
                             .object()
                             .value("root")
                             .toObject()
                             .value("editor")
                             .toString() == QString::fromStdString(unavailableEditorId),
                 60)) {
        return 60;
    }
    if (!require(restored.activeArea()->setEditorId("bloom.viewer") &&
                     restored.activeArea()->areaId() == unavailableAreaId &&
                     restored.activeArea()->editorId() == "bloom.viewer" &&
                     restored.activeArea()->findChild<QLabel*>("unavailableEditorPlaceholder") ==
                         nullptr,
                 61)) {
        return 61;
    }

    const auto duplicateAreaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject duplicateIdState = {
        {"format", "bloom.workspace-layout"},
        {"schema", 1},
        {"root", QJsonObject{{"type", "split"},
                             {"orientation", "horizontal"},
                             {"weights", QJsonArray{0.5, 0.5}},
                             {"children", QJsonArray{QJsonObject{{"type", "area"},
                                                                 {"id", duplicateAreaId},
                                                                 {"editor", "bloom.viewer"},
                                                                 {"active", true}},
                                                     QJsonObject{{"type", "area"},
                                                                 {"id", duplicateAreaId},
                                                                 {"editor", "bloom.nodes"},
                                                                 {"active", false}}}}}},
    };
    if (!require(restored.restoreLayoutState(
                     QJsonDocument(duplicateIdState).toJson(QJsonDocument::Compact)) ==
                         WorkspaceLayoutRestoreResult::Invalid &&
                     restored.areaCount() == 1,
                 62)) {
        return 62;
    }

    const auto firstActiveId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto secondActiveId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject multipleActiveState = {
        {"format", "bloom.workspace-layout"},
        {"schema", 1},
        {"root", QJsonObject{{"type", "split"},
                             {"orientation", "vertical"},
                             {"weights", QJsonArray{0.5, 0.5}},
                             {"children", QJsonArray{QJsonObject{{"type", "area"},
                                                                 {"id", firstActiveId},
                                                                 {"editor", "bloom.viewer"},
                                                                 {"active", true}},
                                                     QJsonObject{{"type", "area"},
                                                                 {"id", secondActiveId},
                                                                 {"editor", "bloom.nodes"},
                                                                 {"active", true}}}}}},
    };
    if (!require(restored.restoreLayoutState(
                     QJsonDocument(multipleActiveState).toJson(QJsonDocument::Compact)) ==
                         WorkspaceLayoutRestoreResult::Invalid &&
                     restored.areaCount() == 1,
                 63)) {
        return 63;
    }

    const QJsonObject invalidIdState = {
        {"format", "bloom.workspace-layout"},
        {"schema", 1},
        {"root", QJsonObject{{"type", "area"},
                             {"id", "not-an-area-id"},
                             {"editor", "bloom.viewer"},
                             {"active", true}}},
    };
    if (!require(restored.restoreLayoutState(
                     QJsonDocument(invalidIdState).toJson(QJsonDocument::Compact)) ==
                         WorkspaceLayoutRestoreResult::Invalid &&
                     restored.areaCount() == 1,
                 64)) {
        return 64;
    }

    const QByteArray corruptState = R"({
        "format":"bloom.workspace-layout","schema":1,"root":{"type":"split"}
    })";
    if (!require(restored.restoreLayoutState(corruptState) ==
                         WorkspaceLayoutRestoreResult::Invalid &&
                     restored.areaCount() == 1,
                 30)) {
        return 30;
    }

    const QByteArray futureState = R"({
        "format":"bloom.workspace-layout","schema":99,"root":{"type":"area"}
    })";
    if (!require(restored.restoreLayoutState(futureState) ==
                         WorkspaceLayoutRestoreResult::UnsupportedVersion &&
                     restored.areaCount() == 1,
                 31)) {
        return 31;
    }
    return 0;
}

int testMainWindow(const EditorRegistry& registry,
                   bloom::ui::CompositionSession& compositionSession) {
    bloom::ui::MainWindow window(registry, compositionSession);
    if (!require(window.windowTitle() == "Bloom" && window.workspaceHost() != nullptr, 40)) {
        return 40;
    }
    if (!require(window.workspaceHost()->areaCount() == 5, 41)) {
        return 41;
    }
    if (!require(window.findChild<QStatusBar*>() == nullptr, 42)) {
        return 42;
    }

    auto* splitAction = window.findChild<QAction*>("splitAreaLeftRightAction");
    auto* closeAction = window.findChild<QAction*>("closeAreaAction");
    auto* maximizeAction = window.findChild<QAction*>("maximizeAreaAction");
    auto* resetAction = window.findChild<QAction*>("resetCompositingLayoutAction");
    auto* undoAction = window.findChild<QAction*>("undoAction");
    auto* redoAction = window.findChild<QAction*>("redoAction");
    if (!require(splitAction != nullptr && closeAction != nullptr && maximizeAction != nullptr &&
                     resetAction != nullptr && undoAction != nullptr && redoAction != nullptr,
                 43)) {
        return 43;
    }
    if (!require(!undoAction->isEnabled() && !redoAction->isEnabled() &&
                     !undoAction->shortcut().isEmpty() && !redoAction->shortcut().isEmpty(),
                 60)) {
        return 60;
    }
    if (!require(closeAction->shortcut().isEmpty() && maximizeAction->shortcut().isEmpty(), 44)) {
        return 44;
    }

    splitAction->trigger();
    if (!require(window.workspaceHost()->areaCount() == 6, 45)) {
        return 45;
    }
    closeAction->trigger();
    if (!require(window.workspaceHost()->areaCount() == 5, 46)) {
        return 46;
    }
    maximizeAction->trigger();
    if (!require(window.workspaceHost()->isAreaMaximized() && maximizeAction->isChecked(), 47)) {
        return 47;
    }
    if (!require(!splitAction->isEnabled() && !closeAction->isEnabled(), 54)) {
        return 54;
    }
    maximizeAction->trigger();
    if (!require(!window.workspaceHost()->isAreaMaximized() && !maximizeAction->isChecked(), 48)) {
        return 48;
    }
    splitAction->trigger();
    resetAction->trigger();
    if (!require(window.workspaceHost()->areaCount() == 5, 55)) {
        return 55;
    }

    QTemporaryDir settingsDirectory;
    if (!require(settingsDirectory.isValid(), 49)) {
        return 49;
    }
    const auto settingsPath = QDir(settingsDirectory.path()).filePath("future-layout.ini");
    const QByteArray futureState = R"({
        "format":"bloom.workspace-layout","schema":99,"root":{"type":"area"}
    })";
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue("workspace/compositing/layout", futureState);
    if (!require(window.restoreApplicationState(settings) ==
                     WorkspaceLayoutRestoreResult::UnsupportedVersion,
                 50)) {
        return 50;
    }
    window.saveApplicationState(settings);
    if (!require(settings.value("workspace/compositing/layout").toByteArray() == futureState, 51)) {
        return 51;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    EditorRegistry registry;

    if (const int result = testRegistry(registry); result != 0) {
        return result;
    }
    if (const int result = testSplitCloseAndActivation(registry); result != 0) {
        return result;
    }
    if (const int result = testMaximizeAndPersistence(registry); result != 0) {
        return result;
    }

    auto newProject = bloom::document::makeNewProject("Window Test", "Composition 1",
                                                      bloom::core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    bloom::document::Document document(std::move(newProject.project));
    bloom::commands::CommandStack commandStack(document);
    bloom::ui::CompositionSession compositionSession(document, commandStack, compositionId);
    return testMainWindow(registry, compositionSession);
}
