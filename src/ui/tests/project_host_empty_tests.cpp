// Blank-start and New Composition duration-unit coverage (task: blank startup + Frames/Seconds).
//
// Opening Bloom (and File > New) must not create a composition: the session starts as a valid,
// clean, editable empty document with no active composition and no staged undo entry, every
// replaceable editor shows the empty state without invalid controls, and the existing New
// Composition command is the one way to author the first one. This file also drives the real
// New Composition dialog end to end to pin the Duration + unit conversion (Frames default,
// Seconds beside it, fps changes, fractional seconds, cancel, inherited duration) against the
// actual command/model result, not a re-implementation.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
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

// A standalone seeded session for the dialog tests, so the current composition's frame rate and
// duration can be controlled exactly.
struct SeededSession final {
    document::Document document;
    commands::CommandStack stack;
    std::optional<ui::CompositionSession> session;

    explicit SeededSession(const document::CompositionFormat format,
                           const core::RationalTime duration)
        : document(document::makeNewProject("Dialog Project", "Main", duration, format).project),
          stack(document), session(std::in_place, document, stack,
                                   document.snapshot().project().compositions().front().id()) {}
};

// Blank session (no composition) for the default-duration tests.
struct BlankSession final {
    document::Document document;
    commands::CommandStack stack;
    std::optional<ui::CompositionSession> session;

    BlankSession()
        : document(document::makeNewProject("Blank Project").project), stack(document),
          session(std::in_place, document, stack, document::CompositionId{}) {}
};

// Finds the live New Composition dialog. QApplication::activeModalWidget() is normally it; the
// top-level fallback keeps the driver robust on the offscreen platform.
[[nodiscard]] QDialog* activeNewCompositionDialog() {
    if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        modal != nullptr && modal->objectName() == QStringLiteral("newCompositionDialog")) {
        return modal;
    }
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("newCompositionDialog")) {
            return qobject_cast<QDialog*>(widget);
        }
    }
    // Naming fallback: still close whatever modal is up rather than let the driver block forever.
    return qobject_cast<QDialog*>(QApplication::activeModalWidget());
}

// Opens the real modal dialog and runs `interact` on it once it is up, then returns the dialog's
// own result (the new CompositionId, or nullopt on cancel/refusal).
[[nodiscard]] std::optional<document::CompositionId>
runNewCompositionDialog(ui::CompositionSession& session,
                        const std::function<void(QDialog&, Expectations&)>& interact,
                        Expectations& expectations) {
    QTimer::singleShot(0, [&interact, &expectations] {
        QDialog* dialog = activeNewCompositionDialog();
        expectations.expect(dialog != nullptr, "the New Composition dialog is up");
        if (dialog == nullptr) {
            return;
        }
        interact(*dialog, expectations);
    });
    return ui::showNewCompositionDialog(session, nullptr);
}

void testBlankStartupOwningSession(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);

    const auto snapshot = host.stateSnapshot();
    expectations.expect(snapshot.valid && snapshot.contentKind ==
                                              host::ProjectSessionContentKind::DecodedDocument,
                        "blank startup: ProjectHost installs a valid decoded document");
    expectations.expect(!host.isDirty() && !snapshot.canUndo && !snapshot.canRedo &&
                            snapshot.historySize == 0,
                        "blank startup: the empty project is clean with zero history (no staged "
                        "undo entry)");
    expectations.expect(!host.lowestCompositionId().isValid(),
                        "blank startup: there is no active composition");
    expectations.expect(host.canSave(),
                        "blank startup: an empty project is still a saveable document");
}

void testEmptyWorkspaceEditorsAndFirstComposition(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    auto [document, stack] = host.liveDocumentAndStack();
    expectations.expect(document != nullptr && stack != nullptr,
                        "empty UI: the blank host exposes a live document/stack");
    if (document == nullptr || stack == nullptr) {
        return;
    }
    ui::CompositionSession session(*document, *stack, host.lowestCompositionId());
    expectations.expect(session.composition() == nullptr,
                        "empty UI: the session has no active composition");

    // Assets editor shows the empty project without an invalid control: the Compositions root
    // exists with zero children, and the existing New Composition action is present.
    ui::AssetsEditor assets(session);
    assets.resize(500, 400);
    assets.show();
    QApplication::processEvents();
    auto* assetsTree = assets.findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
    expectations.expect(assetsTree != nullptr, "empty UI: assets tree exists");
    if (assetsTree != nullptr) {
        expectations.expect(assetsTree->topLevelItemCount() >= 1 &&
                                assetsTree->topLevelItem(0)->text(0) ==
                                    QStringLiteral("Compositions") &&
                                assetsTree->topLevelItem(0)->childCount() == 0,
                            "empty UI: the Compositions root is empty, not stale or invalid");
    }
    expectations.expect(assets.findChild<ui::kit::KIconButton*>(
                            QStringLiteral("assetsNewCompositionButton")) != nullptr,
                        "empty UI: Assets offers the existing New Composition action");
    expectations.expect(
        assets.findChild<ui::kit::KIconButton*>(QStringLiteral("assetsImportButton")) != nullptr,
        "empty UI: Assets still offers Import for a blank project");

    // MainWindow over the same blank session: the workspace (not the read-only placeholder) is
    // authoritative, composition commands are honestly gated, and Save stays available.
    ui::EditorRegistry registry;
    const auto addTestEditor = [&registry](std::string id, QString name) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = [](QWidget* parent) {
                 return new QLabel("empty-state stand-in editor", parent);
             }});
    };
    expectations.expect(addTestEditor("bloom.viewer", "Compositor") &&
                            addTestEditor("bloom.nodes", "Nodes") &&
                            addTestEditor("bloom.timeline", "Timeline") &&
                            addTestEditor("bloom.assets", "Assets") &&
                            addTestEditor("bloom.properties", "Properties"),
                        "empty UI: stand-in editors register");
    runtime::NodeDefinitionRegistry definitions;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    ui::TaskUiBridge bridge(scheduler);
    ui::FrameExportController exporter(session, scheduler, bridge, compiler,
                                       host.publicationCoordinator(), host.artifactCoordinator());
    ui::MainWindow window(registry, session, host, exporter);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    expectations.expect(!window.isShowingReadOnlyPlaceholder(),
                        "empty UI: a blank project shows the workspace, not the placeholder");
    expectations.expect(window.workspaceHost()->isVisible(), "empty UI: the workspace is visible");
    auto* newCompositionAction = window.findChild<QAction*>(QStringLiteral("compositionNewAction"));
    auto* deleteCompositionAction =
        window.findChild<QAction*>(QStringLiteral("compositionDeleteAction"));
    auto* renameCompositionAction =
        window.findChild<QAction*>(QStringLiteral("compositionRenameAction"));
    auto* duplicateCompositionAction =
        window.findChild<QAction*>(QStringLiteral("compositionDuplicateAction"));
    expectations.expect(newCompositionAction != nullptr && newCompositionAction->isEnabled(),
                        "empty UI: the obvious New Composition action is available");
    expectations.expect(
        deleteCompositionAction != nullptr && !deleteCompositionAction->isEnabled() &&
            renameCompositionAction != nullptr && !renameCompositionAction->isEnabled() &&
            duplicateCompositionAction != nullptr && !duplicateCompositionAction->isEnabled(),
        "empty UI: composition actions that need an active composition are disabled, "
        "not left offering an invalid command");
    auto* exportFrameAction = window.findChild<QAction*>(QStringLiteral("exportFrameAction"));
    expectations.expect(exportFrameAction != nullptr && !exportFrameAction->isEnabled(),
                        "empty UI: frame export is disabled with no composition");
    auto* saveProjectAction = window.findChild<QAction*>(QStringLiteral("saveProjectAction"));
    auto* undoAction = window.findChild<QAction*>(QStringLiteral("undoAction"));
    expectations.expect(saveProjectAction != nullptr && saveProjectAction->isEnabled(),
                        "empty UI: the empty project can still be saved");
    expectations.expect(undoAction != nullptr && !undoAction->isEnabled(),
                        "empty UI: there is no undo entry for the startup document");
    if (newCompositionAction == nullptr) {
        return;
    }

    // Creating the first composition through the existing action activates it and re-enables the
    // composition commands.
    bool dialogOpened = false;
    QTimer::singleShot(0, [&] {
        QDialog* dialog = activeNewCompositionDialog();
        expectations.expect(dialog != nullptr, "first composition: dialog opens");
        if (dialog == nullptr) {
            return;
        }
        dialogOpened = true;
        if (auto* ok = dialog->findChild<QPushButton*>(QStringLiteral("assetsDialogOk")))
            ok->click();
        else
            dialog->reject();
    });
    newCompositionAction->trigger();
    QApplication::processEvents();
    expectations.expect(dialogOpened, "first composition: the action drove the real dialog");
    expectations.expect(session.composition() != nullptr,
                        "first composition: creating the first composition activates it");
    // Delete stays disabled for the FINAL composition: the command layer protects the last one
    // (src/commands/operations.cpp DeleteComposition), so offering it here would promise a
    // command that refuses. Renaming/duplicating become available.
    expectations.expect(
        renameCompositionAction != nullptr && renameCompositionAction->isEnabled() &&
            duplicateCompositionAction != nullptr && duplicateCompositionAction->isEnabled(),
        "first composition: Rename/Duplicate enable once an active composition "
        "exists");
    if (assetsTree != nullptr) {
        expectations.expect(assetsTree->topLevelItem(0)->childCount() == 1,
                            "first composition: Assets lists the new composition");
    }
}

void testDurationDefaultsToTenSecondsInFrames(Expectations& expectations) {
    BlankSession fixture;
    const auto id = runNewCompositionDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration =
                dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
            auto* unit =
                dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsDurationUnitDropdown"));
            auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
            expect.expect(duration != nullptr && unit != nullptr && ok != nullptr,
                          "default duration: controls exist");
            if (duration != nullptr) {
                expect.expect(std::abs(duration->value() - 240.0) < 0.001,
                              "default duration: 10 s at 24 fps is 240 "
                              "frames, never 10 frames");
                expect.expect(duration->decimals() == 0, "default duration: Frames are integer");
            }
            if (unit != nullptr) {
                expect.expect(unit->currentIndex() == 0, "default duration: Frames is the default");
            }
            if (ok != nullptr) {
                ok->click();
            } else {
                dialog.reject();
            }
        },
        expectations);
    expectations.expect(id.has_value(), "default duration: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    const auto expected = core::RationalTime::create(240, 24);
    expectations.expect(composition != nullptr && expected.has_value() &&
                            composition->duration() == *expected,
                        "default duration: the stored duration is exactly 10 s");
}

void testFramesAtTwentyFiveAndThirtyFps(Expectations& expectations) {
    for (const std::uint32_t fps : {25U, 30U}) {
        BlankSession fixture;
        const auto id = runNewCompositionDialog(
            *fixture.session,
            [fps](QDialog& dialog, Expectations& expect) {
                auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("assetsFrameRateField"));
                auto* duration =
                    dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
                auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
                if (rate == nullptr || duration == nullptr || ok == nullptr) {
                    expect.expect(false, "fps duration: controls exist");
                    dialog.reject();
                    return;
                }
                rate->setValue(static_cast<int>(fps));
                duration->setValue(240.0);
                ok->click();
            },
            expectations);
        expectations.expect(id.has_value(), "fps duration: the dialog adds a composition");
        if (!id.has_value()) {
            continue;
        }
        const auto* composition = fixture.document.snapshot().project().findComposition(*id);
        const auto expected = core::RationalTime::create(240, fps);
        expectations.expect(composition != nullptr && expected.has_value() &&
                                composition->duration() == *expected,
                            "fps duration: 240 frames at the chosen fps stores frames/fps");
    }
}

void testFractionalSeconds(Expectations& expectations) {
    BlankSession fixture;
    const auto id = runNewCompositionDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit =
                dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsDurationUnitDropdown"));
            auto* duration =
                dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
            auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("assetsFrameRateField"));
            auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
            if (unit == nullptr || duration == nullptr || rate == nullptr || ok == nullptr) {
                expect.expect(false, "fractional seconds: controls exist");
                dialog.reject();
                return;
            }
            unit->setCurrentIndex(1);
            expect.expect(duration->decimals() > 0, "fractional seconds: Seconds accept fractions");
            duration->setValue(10.5);
            rate->setValue(25);
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "fractional seconds: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    const auto expected = core::RationalTime::create(21, 2);
    expectations.expect(composition != nullptr && expected.has_value() &&
                            composition->duration() == *expected,
                        "fractional seconds: 10.5 s is stored exactly as 21/2 s");
}

void testSwitchUnitsPreservesDuration(Expectations& expectations) {
    BlankSession fixture;
    const auto id = runNewCompositionDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit =
                dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsDurationUnitDropdown"));
            auto* duration =
                dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
            auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
            if (unit == nullptr || duration == nullptr || ok == nullptr) {
                expect.expect(false, "switch units: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(std::abs(duration->value() - 240.0) < 0.001,
                          "switch units: starts at 240 frames");
            unit->setCurrentIndex(1);
            expect.expect(std::abs(duration->value() - 10.0) < 0.000001,
                          "switch units: Frames -> Seconds shows 10 s");
            unit->setCurrentIndex(0);
            expect.expect(std::abs(duration->value() - 240.0) < 0.001,
                          "switch units: Seconds -> Frames restores 240 "
                          "frames, same length");
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "switch units: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    const auto expected = core::RationalTime::create(240, 24);
    expectations.expect(composition != nullptr && expected.has_value() &&
                            composition->duration() == *expected,
                        "switch units: the resulting duration is unchanged (10 s)");
}

void testFpsChangeKeepsEnteredUnit(Expectations& expectations) {
    // Frames stay frames: 240 frames at 30 fps is 8 s.
    {
        BlankSession fixture;
        const auto id = runNewCompositionDialog(
            *fixture.session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("assetsFrameRateField"));
                auto* duration =
                    dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
                auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
                if (rate == nullptr || duration == nullptr || ok == nullptr) {
                    expect.expect(false, "fps frames: controls exist");
                    dialog.reject();
                    return;
                }
                rate->setValue(30);
                expect.expect(std::abs(duration->value() - 240.0) < 0.001,
                              "fps frames: the frame count is unchanged");
                ok->click();
            },
            expectations);
        const auto expected = core::RationalTime::create(240, 30);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && expected.has_value() &&
                                composition->duration() == *expected,
                            "fps frames: the duration follows frames/new fps (8 s)");
    }
    // Seconds stay seconds: 10 s at 30 fps is still 10 s.
    {
        BlankSession fixture;
        const auto id = runNewCompositionDialog(
            *fixture.session,
            [](QDialog& dialog, Expectations& expect) {
                auto* unit = dialog.findChild<ui::kit::KDropdown*>(
                    QStringLiteral("assetsDurationUnitDropdown"));
                auto* duration =
                    dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
                auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("assetsFrameRateField"));
                auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
                if (unit == nullptr || duration == nullptr || rate == nullptr || ok == nullptr) {
                    expect.expect(false, "fps seconds: controls exist");
                    dialog.reject();
                    return;
                }
                unit->setCurrentIndex(1);
                duration->setValue(10.0);
                rate->setValue(30);
                expect.expect(std::abs(duration->value() - 10.0) < 0.000001,
                              "fps seconds: the seconds value is unchanged");
                ok->click();
            },
            expectations);
        const auto expected = core::RationalTime::create(10, 1);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && expected.has_value() &&
                                composition->duration() == *expected,
                            "fps seconds: the duration stays the entered seconds");
    }
}

void testInvalidValuesAreClamped(Expectations& expectations) {
    BlankSession fixture;
    const auto id = runNewCompositionDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration =
                dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
            auto* unit =
                dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsDurationUnitDropdown"));
            auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
            if (duration == nullptr || unit == nullptr || ok == nullptr) {
                expect.expect(false, "invalid values: controls exist");
                dialog.reject();
                return;
            }
            duration->setValue(-5.0);
            expect.expect(duration->value() >= 1.0, "invalid values: Frames clamp at a positive "
                                                    "integer, never zero or negative");
            unit->setCurrentIndex(1);
            duration->setValue(0.0);
            expect.expect(duration->value() > 0.0, "invalid values: Seconds clamp above zero");
            unit->setCurrentIndex(0);
            expect.expect(ok->isEnabled(), "invalid values: OK stays live because the field "
                                           "cannot hold an invalid duration");
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "invalid values: a clamped valid duration still commits");
}

void testCancelAddsNothing(Expectations& expectations) {
    BlankSession fixture;
    const auto before = fixture.document.snapshot().project().compositions().size();
    const auto id = runNewCompositionDialog(
        *fixture.session, [](QDialog& dialog, Expectations&) { dialog.reject(); }, expectations);
    expectations.expect(!id.has_value(), "cancel: the dialog returns no composition");
    expectations.expect(fixture.document.snapshot().project().compositions().size() == before,
                        "cancel: no composition was added and no command ran");
}

void testInheritsCurrentCompositionDuration(Expectations& expectations) {
    const auto format = document::CompositionFormat::create(
        1920, 1080, core::PixelAspectRatio::square(), document::FrameRate::create(25, 1).value());
    const auto duration = core::RationalTime::create(5, 1);
    expectations.expect(format.has_value() && duration.has_value(),
                        "inherit duration: fixture format/duration are valid");
    if (!format.has_value() || !duration.has_value()) {
        return;
    }
    SeededSession fixture(*format, *duration);
    expectations.expect(fixture.session->composition() != nullptr,
                        "inherit duration: the seeded session has a composition");
    const auto id = runNewCompositionDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration =
                dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
            auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("assetsFrameRateField"));
            auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
            if (duration == nullptr || rate == nullptr || ok == nullptr) {
                expect.expect(false, "inherit duration: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(rate->value() == 25, "inherit duration: fps inherits the current "
                                               "composition");
            expect.expect(std::abs(duration->value() - 125.0) < 0.001,
                          "inherit duration: 5 s at 25 fps shows as 125 "
                          "frames");
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "inherit duration: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    expectations.expect(composition != nullptr && composition->duration() == *duration,
                        "inherit duration: the new composition inherits the current duration");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;

    testBlankStartupOwningSession(expectations);
    testEmptyWorkspaceEditorsAndFirstComposition(expectations);
    testDurationDefaultsToTenSecondsInFrames(expectations);
    testFramesAtTwentyFiveAndThirtyFps(expectations);
    testFractionalSeconds(expectations);
    testSwitchUnitsPreservesDuration(expectations);
    testFpsChangeKeepsEnteredUnit(expectations);
    testInvalidValuesAreClamped(expectations);
    testCancelAddsNothing(expectations);
    testInheritsCurrentCompositionDuration(expectations);

    return expectations.failures() == 0 ? 0 : 1;
}
