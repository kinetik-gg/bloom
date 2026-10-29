// Lane C review captures: every Bloom dialog, opened through its real entry point over the
// main-window fixture where one exists, grabbed offscreen, and audited against the one-control-
// height rule (ui-grammar.md: every field, dropdown and button Size::Control tall), the shared
// label column of its forms and the dialog margins. Native platform file dialogs are not captured:
// an offscreen platform cannot show them.
//
// Usage: bloom_dialog_capture <directory> <1|1.5>
// Writes <directory>/<name>-dpr1.png or -dpr15.png and prints one audit line per dialog; the exit
// status is the number of audit findings.
#include "window_fixture.hpp"

#include "composition_export_dialog.hpp"
#include "composition_settings_dialog.hpp"
#include "import_progress_dialog.hpp"
#include "node_editor_add.hpp"
#include "project_item_delete.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/h264_encoder_service.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <bloom/ui/kit/file_chooser.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::ui;

QString gDirectory;
QString gSuffix;
// The x of the Preferences label column, shared by every page.
int gPreferencesLabelX = -1;
int gFindings = 0;

void settle(const int ms = 150) {
    QTest::qWait(ms);
    QApplication::processEvents();
}

// The one-height rule, the shared label column and the margins.
void audit(const QString& name, QWidget& root) {
    QStringList issues;
    const int control = kit::px(kit::Size::Control);
    for (auto* widget : root.findChildren<QWidget*>()) {
        if (!widget->isVisibleTo(&root))
            continue;
        // A value field's own editor and a dropdown's popup parts are inside their control.
        if (qobject_cast<kit::KValueField*>(widget->parentWidget()) != nullptr)
            continue;
        // Bloom's file dialog: Qt's own controls, styled and sized by the kit.
        const bool fileDialogControl =
            qobject_cast<QFileDialog*>(&root) != nullptr &&
            (qobject_cast<QComboBox*>(widget) != nullptr ||
             (qobject_cast<QLineEdit*>(widget) != nullptr &&
              qobject_cast<QComboBox*>(widget->parentWidget()) == nullptr) ||
             qobject_cast<QPushButton*>(widget) != nullptr ||
             (qobject_cast<QToolButton*>(widget) != nullptr && !widget->objectName().isEmpty()));
        const bool governed = fileDialogControl || qobject_cast<kit::KButton*>(widget) != nullptr ||
                              qobject_cast<kit::KIconButton*>(widget) != nullptr ||
                              qobject_cast<kit::KDropdown*>(widget) != nullptr ||
                              qobject_cast<kit::KValueField*>(widget) != nullptr ||
                              qobject_cast<kit::KColorChip*>(widget) != nullptr ||
                              qobject_cast<kit::KLineEdit*>(widget) != nullptr;
        if (governed && widget->height() != control)
            issues << QStringLiteral("%1 '%2' is %3 px tall, not %4")
                          .arg(QString::fromLatin1(widget->metaObject()->className()),
                               widget->objectName())
                          .arg(widget->height())
                          .arg(control);
    }
    int labelWidth = -1;
    for (auto* form : root.findChildren<kit::KForm*>()) {
        auto* grid = qobject_cast<QGridLayout*>(form->layout());
        if (grid == nullptr)
            continue;
        for (int row = 0; row < grid->rowCount(); ++row) {
            auto* item = grid->itemAtPosition(row, 0);
            auto* label = item != nullptr ? qobject_cast<kit::KLabel*>(item->widget()) : nullptr;
            if (label == nullptr || item->widget() != label ||
                grid->itemAtPosition(row, 1) == item || !label->isVisibleTo(&root))
                continue;
            if (label->minimumWidth() != label->maximumWidth())
                issues << QStringLiteral("label '%1' is not in the fixed label column")
                              .arg(label->text());
            if (labelWidth < 0)
                labelWidth = label->width();
            else if (label->width() != labelWidth)
                issues << QStringLiteral("label '%1' is %2 px wide; the column is %3")
                              .arg(label->text())
                              .arg(label->width())
                              .arg(labelWidth);
        }
    }
    // Preferences is a Properties-style settings window: every row's label is the Properties
    // row label, in the one label column all its pages share.
    if (root.objectName() == QStringLiteral("preferencesWindow"))
        for (auto* row : root.findChildren<kit::KPropertyRow*>()) {
            if (!row->isVisibleTo(&root))
                continue;
            auto* label = row->findChild<QLabel*>(QStringLiteral("propertiesRowLabel"),
                                                  Qt::FindDirectChildrenOnly);
            if (label == nullptr || label->width() != kit::px(kit::Size::PreferencesLabelWidth)) {
                issues << QStringLiteral("a Preferences row label is not in the shared column");
                continue;
            }
            // One column on every page: the same x in the window on each of them.
            const int x = label->mapTo(&root, QPoint()).x();
            if (gPreferencesLabelX < 0)
                gPreferencesLabelX = x;
            else if (x != gPreferencesLabelX)
                issues << QStringLiteral("a Preferences row label column starts at another x");
        }
    if (auto* dialog = qobject_cast<kit::KDialog*>(&root); dialog && dialog->layout()) {
        const auto margins = dialog->layout()->contentsMargins();
        const int m = kit::px(kit::Spacing::M);
        if (margins.left() != m || margins.top() != m || margins.right() != m ||
            margins.bottom() != m)
            issues << QStringLiteral("dialog margins are not Spacing::M");
    }
    gFindings += static_cast<int>(issues.size());
    std::cout << name.toStdString() << ": "
              << (issues.isEmpty() ? std::string("ok") : issues.join("; ").toStdString()) << '\n';
}

void shoot(const QString& name, QWidget& widget) {
    settle();
    audit(name, widget);
    const auto path =
        QDir(gDirectory).filePath(name + QStringLiteral("-dpr") + gSuffix + QStringLiteral(".png"));
    if (!widget.grab().save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFindings;
    }
}

[[nodiscard]] QDialog* topModal() {
    return qobject_cast<QDialog*>(QApplication::activeModalWidget());
}

[[nodiscard]] kit::KDialog* visible(const QString& objectName) {
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* dialog = qobject_cast<kit::KDialog*>(widget);
            dialog && dialog->isVisible() && dialog->objectName() == objectName)
            return dialog;
    return nullptr;
}

// Captures the next modal (an exec() dialog) once it is up: `prepare` sets it up, `after` runs
// once it is captured (to open a nested prompt), then it is rejected.
void onModal(const QString& name, std::function<void(QDialog&)> prepare = {},
             std::function<void(QDialog&)> after = {}) {
    QTimer::singleShot(0, [name, prepare = std::move(prepare), after = std::move(after)] {
        QDialog* dialog = nullptr;
        (void)QTest::qWaitFor([&dialog] { return (dialog = topModal()) != nullptr; }, 3000);
        if (dialog == nullptr) {
            std::cerr << name.toStdString() << ": no modal dialog appeared\n";
            ++gFindings;
            return;
        }
        if (prepare)
            prepare(*dialog);
        // An empty name captures nothing itself; `after` captures the states it walks through.
        if (!name.isEmpty())
            shoot(name, *dialog);
        if (after)
            after(*dialog);
        dialog->reject();
    });
}

void shootAsync(const QString& name, const QString& objectName,
                const std::function<void(kit::KDialog&)>& prepare = {}) {
    kit::KDialog* dialog = nullptr;
    (void)QTest::qWaitFor([&] { return (dialog = visible(objectName)) != nullptr; }, 3000);
    if (dialog == nullptr) {
        std::cerr << name.toStdString() << ": " << objectName.toStdString() << " did not open\n";
        ++gFindings;
        return;
    }
    if (prepare)
        prepare(*dialog);
    shoot(name, *dialog);
    dialog->reject();
    settle(50);
}

template <class T> T* child(QWidget& root, const char* name) {
    return root.findChild<T*>(QString::fromLatin1(name));
}

void trigger(QWidget& root, const char* action) {
    if (auto* found = root.findChild<QAction*>(QString::fromLatin1(action)))
        found->trigger();
    else {
        std::cerr << "missing action " << action << '\n';
        ++gFindings;
    }
}

void compositionDialogs(test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    const auto showTab = [](QDialog& dialog, const int index) {
        child<kit::KTabs>(dialog, "compositionSettingsTabs")->setCurrentIndex(index);
        settle(200);
    };
    onModal(QStringLiteral("new-composition"), [showTab](QDialog& dialog) { showTab(dialog, 0); });
    trigger(window, "compositionNewAction");
    onModal(QStringLiteral("new-composition-advanced"),
            [showTab](QDialog& dialog) { showTab(dialog, 1); });
    trigger(window, "compositionNewAction");
    // A refused value on Advanced, seen from Basic: the Error dot on the Advanced tab.
    onModal(QStringLiteral("new-composition-advanced-error"), [showTab](QDialog& dialog) {
        showTab(dialog, 0);
        child<kit::KValueField>(dialog, "compositionSettingsTitleSafe")->setValue(95);
    });
    trigger(window, "compositionNewAction");
    onModal(QStringLiteral("new-composition-advanced-error-shown"), [showTab](QDialog& dialog) {
        showTab(dialog, 0);
        child<kit::KValueField>(dialog, "compositionSettingsTitleSafe")->setValue(95);
        if (auto* kit = qobject_cast<kit::KDialog*>(&dialog))
            (void)kit->activate(kit::KDialog::Role::Accept);
    });
    trigger(window, "compositionNewAction");
    onModal(QStringLiteral("composition-settings"),
            [showTab](QDialog& dialog) { showTab(dialog, 0); });
    trigger(window, "compositionSettingsAction");
    onModal(QStringLiteral("composition-settings-advanced"), [showTab](QDialog& dialog) {
        showTab(dialog, 1);
        // A size change offers the resize anchor.
        child<kit::KValueField>(dialog, "compositionSettingsWidth")->setValue(2048);
        settle(100);
    });
    trigger(window, "compositionSettingsAction");
    onModal(QStringLiteral("composition-settings-error"), [showTab](QDialog& dialog) {
        showTab(dialog, 0);
        auto* rates = child<kit::KDropdown>(dialog, "compositionSettingsRatePreset");
        rates->setCurrentIndex(rates->count() - 1);
        child<QLineEdit>(dialog, "compositionSettingsRate")->setText(QStringLiteral("0"));
    });
    trigger(window, "compositionSettingsAction");
    // Save as preset…, then Rename preset… on the preset it saved.
    onModal(QStringLiteral("composition-settings-presets"), {}, [](QDialog& dialog) {
        onModal(QStringLiteral("preset-save-prompt"));
        child<QAbstractButton>(dialog, "compositionSettingsSavePreset")->click();
        if (auto* settings = dynamic_cast<CompositionSettingsDialog*>(&dialog)) {
            const auto refused = settings->savePreset(QStringLiteral("Review 1080"));
            settle(50);
            auto* rename = child<QAbstractButton>(dialog, "compositionSettingsRenamePreset");
            if (refused || !rename->isEnabled()) {
                std::cerr << "preset rename: "
                          << (refused ? refused->toStdString() : std::string("disabled")) << '\n';
                ++gFindings;
            }
            // Scheduled only now, so the settle above cannot answer it with this dialog.
            onModal(QStringLiteral("preset-rename-prompt"), [](QDialog& prompt) {
                if (auto* field = prompt.findChild<QLineEdit*>())
                    field->setText(QString{});
            });
            rename->click();
            (void)settings->deleteSelectedPreset();
        }
    });
    trigger(window, "compositionSettingsAction");
    trigger(window, "compositionRenameAction");
    shootAsync(QStringLiteral("rename-composition"), QStringLiteral("compositionRenamePrompt"));
}

void exportDialogs(test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    const bool h264 =
        output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available;
    std::atomic<bool> releaseLocate = false;
    H264EncoderService encoder(
        fixture.scheduler, fixture.bridge,
        {.software = output::H264EncoderV1::OpenH264, .hardwareVaapi = true, .softwareReason = {}},
        [](const output::H264EncoderSupportV1&) {
            return H264EncoderProbe{.vaapiDevice = true,
                                    .openh264Installed = false,
                                    .detail = "H.264 encoder not installed"};
        },
        [&releaseLocate](const std::filesystem::path&, const std::function<bool()>& cancelled) {
            while (!releaseLocate && !cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return output::H264RuntimeAvailabilityV1{};
        });
    CompositionExportDialogOptions options;
    options.maximumFrame = 239;
    options.sampleRate = 48000;
    options.colorSettings = &fixture.session.colorSettings();
    options.encoder = &encoder;
    const auto chooseH264 = [h264, &encoder](QDialog& dialog) {
        (void)QTest::qWaitFor([&encoder] { return encoder.state().probed; }, 3000);
        auto* preset = child<kit::KDropdown>(dialog, "compositionExportPreset");
        const auto index = preset->findData(static_cast<int>(output::OutputPresetV1::H264MovV1));
        if (h264 && index >= 0)
            preset->chooseIndex(index);
        child<QLineEdit>(dialog, "compositionExportDestination")
            ->setText(QStringLiteral("review.mov"));
    };
    onModal(QStringLiteral("export-composition-consent"), chooseH264, [](QDialog& dialog) {
        onModal(QStringLiteral("openh264-consent"));
        child<QAbstractButton>(dialog, "installOpenH264")->click();
    });
    (void)compositionExportDialog(window, options);
    onModal(QStringLiteral("export-composition-locating"), [&](QDialog& dialog) {
        chooseH264(dialog);
        (void)encoder.locate("libopenh264.so.8");
        (void)QTest::qWaitFor([&encoder] { return encoder.locating(); }, 3000);
    });
    (void)compositionExportDialog(window, options);
    releaseLocate = true;
    (void)QTest::qWaitFor([&encoder] { return !encoder.locating(); }, 3000);
    onModal(QStringLiteral("export-composition-error"), [](QDialog& dialog) {
        child<kit::KValueField>(dialog, "compositionExportFirst")->setValue(200);
        child<kit::KValueField>(dialog, "compositionExportLast")->setValue(20);
        child<QLineEdit>(dialog, "compositionExportDestination")
            ->setText(QStringLiteral("shot.exr"));
    });
    (void)compositionExportDialog(window, options);

    {
        const std::unique_ptr<kit::KDialog> range(
            FrameExportController::buildRangeDialog(window, 239));
        range->show();
        shoot(QStringLiteral("export-frame-range"), *range);
    }
    {
        FrameExportApprovalPrompt prompt;
        prompt.destination = std::filesystem::path("renders") / "shot_010.0001.png";
        prompt.width = 1920;
        prompt.height = 1080;
        prompt.preset = output::OutputPresetV1::PngRgba8SrgbV1;
        prompt.presetName = QStringLiteral("PngRgba8SrgbV1");
        prompt.presetLabel = outputPresetLabel(prompt.preset);
        prompt.facets.exactFacetCount = 9;
        prompt.facets.nonExactFacetCount = 2;
        prompt.facets.nonExactFacetNames = {QStringLiteral("Precision"), QStringLiteral("Colour")};
        prompt.digestShortForm = QStringLiteral("3f2a9c0d81b4e6f7");
        prompt.implementationNote = QStringLiteral("look: baked (1 look-tagged effect)");
        const std::unique_ptr<kit::KDialog> approval(
            FrameExportController::buildApprovalDialog(window, prompt));
        approval->show();
        shoot(QStringLiteral("approve-export"), *approval);
    }
}

void fileChoosers(test::WindowFixture& fixture, const QString& root) {
    auto& window = *fixture.window;
    // Its own folder, so the files shown here never meet the import below.
    const auto scratch = QDir(root).filePath(QStringLiteral("browse"));
    QDir().mkpath(scratch);
    QImage image(16, 9, QImage::Format_RGBA8888);
    image.fill(QColor(80, 120, 160));
    for (const char* name : {"plate_010.png", "plate_020.png", "shot_010.bloom"})
        (void)image.save(QDir(scratch).filePath(QString::fromLatin1(name)), "PNG");
    QDir(scratch).mkpath(QStringLiteral("renders"));
    const auto open = kit::buildFileDialog(
        window, {.mode = kit::KFileRequest::Mode::Open,
                 .title = QObject::tr("Open Project"),
                 .kind = QStringLiteral("capture"),
                 .nameFilters = QObject::tr("Bloom Projects (*.bloom);;All files (*)"),
                 .defaultSuffix = QStringLiteral("bloom"),
                 .startPath = scratch});
    open->show();
    settle(400);
    shoot(QStringLiteral("file-chooser-open"), *open);
    open->reject();
    const auto save = kit::buildFileDialog(
        window, {.mode = kit::KFileRequest::Mode::Save,
                 .title = QObject::tr("Export Frame"),
                 .kind = QStringLiteral("capture"),
                 .nameFilters = QObject::tr("OpenEXR (*.exr);;PNG (*.png);;TIFF (*.tif *.tiff)"),
                 .defaultSuffix = {},
                 .startPath = scratch});
    save->selectFile(QStringLiteral("shot_010.exr"));
    save->show();
    settle(400);
    shoot(QStringLiteral("file-chooser-save"), *save);
    save->reject();
}

void projectDialogs(test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    {
        const std::unique_ptr<kit::KDialog> unsaved(ProjectHost::buildUnsavedChangesPrompt(
            window, QStringLiteral("Shot 12"), UnsavedChangeAction::OpenProject));
        unsaved->show();
        shoot(QStringLiteral("unsaved-changes"), *unsaved);
    }
    {
        const std::unique_ptr<kit::KDialog> recover(
            ProjectHost::buildRecoveryPrompt(window, "shot-12.recovery.bloom"));
        recover->show();
        shoot(QStringLiteral("recover"), *recover);
    }
    // A failure notice through MainWindow's own path.
    emit fixture.projectHost.saveFinished(
        ProjectHostOperationOutcome::Failed,
        QStringLiteral("The disk is full. Free some space and save again."));
    shootAsync(QStringLiteral("failure-inform"), QStringLiteral("projectSaveFailed"));
    trigger(window, "projectColorSettingsAction");
    shootAsync(QStringLiteral("project-colour"), QStringLiteral("projectColorSettingsDialog"));
    // Preferences: every page.
    onModal(QString{}, {}, [&window](QDialog& dialog) {
        auto* rail = child<QListWidget>(dialog, "preferencesCategoryRail");
        if (rail == nullptr) {
            std::cerr << "preferences: no page rail\n";
            ++gFindings;
            return;
        }
        const auto pageName = [rail](const int row) {
            return QStringLiteral("preferences-") +
                   rail->item(row)
                       ->text()
                       .toLower()
                       .remove(QLatin1Char('&'))
                       .simplified()
                       .replace(QLatin1Char(' '), QLatin1Char('-'));
        };
        for (int row = 0; row < rail->count(); ++row) {
            if (rail->item(row)->flags() == Qt::NoItemFlags)
                continue;
            rail->setCurrentRow(row);
            shoot(pageName(row), dialog);
        }
        // The same window at its narrowest.
        const auto size = dialog.size();
        for (int row = 0; row < rail->count(); ++row)
            if (rail->item(row)->text() == QStringLiteral("Memory & Caches"))
                rail->setCurrentRow(row);
        dialog.resize(dialog.minimumSizeHint().width(), dialog.height());
        shoot(QStringLiteral("preferences-narrow"), dialog);
        dialog.resize(size);
        settle(100);
        // A Properties section beside a Preferences section: one component, one look.
        auto* properties = window.findChild<QWidget*>(QStringLiteral("propertiesEditor"));
        auto* propertiesSection =
            properties != nullptr ? properties->findChild<kit::KSection*>() : nullptr;
        auto* preferencesSection = child<kit::KSection>(dialog, "preferencesMemorySection");
        if (propertiesSection == nullptr || preferencesSection == nullptr) {
            std::cerr << "side by side: a section is missing\n";
            ++gFindings;
            return;
        }
        // Both at the capture's device pixels, side by side and unscaled.
        auto left = propertiesSection->grab().toImage();
        auto right = preferencesSection->grab().toImage();
        const int gap =
            static_cast<int>(std::lround(kit::px(kit::Spacing::L) * left.devicePixelRatio()));
        left.setDevicePixelRatio(1.0);
        right.setDevicePixelRatio(1.0);
        QImage combined(left.width() + gap + right.width(), std::max(left.height(), right.height()),
                        QImage::Format_ARGB32);
        combined.fill(kit::color(kit::Color::Background));
        QPainter painter(&combined);
        painter.drawImage(0, 0, left);
        painter.drawImage(left.width() + gap, 0, right);
        painter.end();
        (void)combined.save(QDir(gDirectory)
                                .filePath(QStringLiteral("preferences-vs-properties-dpr") +
                                          gSuffix + QStringLiteral(".png")));
    });
    trigger(window, "preferencesAction");
    trigger(window, "openSourceLicensesAction");
    shootAsync(QStringLiteral("licenses"), QStringLiteral("licensesWindow"));
}

void importDialogs(test::WindowFixture& fixture) {
    ImportProgressDialog dialog(*fixture.window);
    AssetController::ImportProgressState state;
    state.finding = false;
    state.completed = 1;
    state.total = 5;
    state.workDone = 140;
    state.workTotal = 600;
    state.current = QStringLiteral("SC020_SH070_image_0001.png");
    state.frames = 522;
    state.framesRead = 119;
    dialog.showProgress(state);
    dialog.show();
    shoot(QStringLiteral("import-progress-running"), dialog);
    AssetController::ImportReport report;
    report.imported = 10;
    report.skipped = {
        {QStringLiteral("broken.png"), QStringLiteral("The file is not a readable image.")},
        {QStringLiteral("SC020_SH070_reference_plate_with_a_very_long_name_v012.exr"),
         QStringLiteral("Already in the project")}};
    report.cancelled = true;
    report.notImported = 9;
    dialog.showSummary(report);
    shoot(QStringLiteral("import-progress-summary"), dialog);
    dialog.dismiss();
}

[[nodiscard]] std::optional<document::AssetId> importAsset(test::WindowFixture& fixture,
                                                           const QString& directory) {
    const auto path = QDir(directory).filePath(QStringLiteral("plate_010.png"));
    QImage image(16, 9, QImage::Format_RGBA8888);
    image.fill(QColor(80, 120, 160));
    if (!image.save(path))
        return std::nullopt;
    fixture.assets->importFiles({path});
    (void)QTest::qWaitFor([&] { return !fixture.assets->busy(); }, 10000);
    for (const auto& asset : fixture.session.snapshot().project().assets())
        if (asset.kind == document::AssetKind::Image && asset.name.starts_with("plate_010"))
            return asset.id;
    return std::nullopt;
}

void assetDialogs(test::WindowFixture& fixture, const QString& scratch) {
    auto* editor = fixture.window->findChild<AssetsEditor*>();
    auto* tree = editor ? editor->findChild<QTreeWidget*>(QStringLiteral("assetsTree")) : nullptr;
    const auto asset = importAsset(fixture, scratch);
    if (tree == nullptr || !asset) {
        std::cerr << "assets: no tree or no imported asset\n";
        ++gFindings;
        return;
    }
    // Used, inside a folder: the full delete warning.
    const auto composition = fixture.session.compositionId();
    {
        commands::Transaction use("Use plate", fixture.session.snapshot().revision());
        use.emplace<commands::AddImageLayer>(composition, *asset);
        (void)fixture.session.executeTransaction(std::move(use));
    }
    std::optional<document::AssetFolderId> folder;
    {
        commands::Transaction create("Folder", fixture.session.snapshot().revision());
        create.emplace<commands::CreateAssetFolder>("Plates");
        const auto result = fixture.session.executeTransaction(std::move(create));
        folder = result.outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    }
    if (folder) {
        commands::Transaction move("Move", fixture.session.snapshot().revision());
        move.emplace<commands::MoveAssets>(std::vector{*asset}, folder, 0);
        (void)fixture.session.executeTransaction(std::move(move));
    }
    settle(300);
    const auto itemFor = [tree](const int role, const std::uint64_t id) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, role).toULongLong() == id)
                return *it;
        return nullptr;
    };
    const auto context = [tree](QTreeWidgetItem* item, const char* name) {
        if (item == nullptr)
            return;
        tree->clearSelection();
        item->setSelected(true);
        tree->setCurrentItem(item);
        QTimer::singleShot(0, tree, [name] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr)
                return;
            QAction* chosen = nullptr;
            for (auto* action : menu->actions())
                if (action->objectName() == QString::fromLatin1(name))
                    chosen = action;
            menu->close();
            if (chosen)
                chosen->trigger();
        });
        emit tree->customContextMenuRequested(tree->visualItemRect(item).center());
        QApplication::processEvents();
    };
    constexpr int kAssetRole = Qt::UserRole + 2;
    context(itemFor(kAssetRole, asset->value()), "assetsEditTagsAction");
    shootAsync(QStringLiteral("edit-tags"), QStringLiteral("assetsTagsDialog"));
    context(itemFor(kAssetRole, asset->value()), "assetsInputColorAction");
    shootAsync(QStringLiteral("input-colour"), QStringLiteral("assetsInputColorDialog"));
    if (folder) {
        ProjectItemSelection selection;
        selection.folders = {*folder};
        requestDeleteProjectItems(fixture.session, *fixture.window, std::move(selection));
        shootAsync(QStringLiteral("delete-confirm"), QStringLiteral("assetsDeleteConfirmation"));
    }
}

void compositionDeleteAndViewer(test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto& session = fixture.session;
    const auto outer = session.compositionId();
    commands::Transaction create("Nested", session.snapshot().revision());
    create.emplace<commands::AddComposition>("Title Card", document::CompositionFormat{},
                                             core::RationalTime::fromInteger(3));
    const auto created = session.executeTransaction(std::move(create));
    const auto nested = created.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    if (!nested) {
        ++gFindings;
        return;
    }
    commands::Transaction use("Use nested", session.snapshot().revision());
    use.emplace<node_editor::AddCompositionSource>(outer, *nested, document::Vec2d{}, true);
    (void)session.executeTransaction(std::move(use));
    (void)session.setComposition(*nested);
    settle(200);
    trigger(window, "compositionDeleteAction");
    shootAsync(QStringLiteral("delete-composition-in-use"),
               QStringLiteral("assetsDeleteConfirmation"));
    (void)session.setComposition(outer);
    settle(300);
    trigger(window, "viewerCustomSafeAreasAction");
    shootAsync(QStringLiteral("custom-safe-areas"), QStringLiteral("viewerSafeAreaDialog"));
}

void labelColourPicker(test::WindowFixture& fixture) {
    auto* stack = fixture.window->findChild<TimelineLayerStack*>();
    if (stack == nullptr || stack->rowCount() == 0) {
        std::cerr << "label colour: no timeline layer rows\n";
        ++gFindings;
        return;
    }
    const QPoint at(stack->width() / 2, stack->rowTop(0) + kTimelineRowHeight / 2);
    QContextMenuEvent event(QContextMenuEvent::Mouse, at, stack->mapToGlobal(at));
    QApplication::sendEvent(stack, &event);
    settle(100);
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    QAction* custom = nullptr;
    for (auto* action : menu != nullptr ? menu->actions() : QList<QAction*>{})
        if (auto* sub = action->menu(); sub && action->text() == QObject::tr("Label Color"))
            for (auto* entry : sub->actions())
                if (entry->text() == QObject::tr("Custom…"))
                    custom = entry;
    if (menu != nullptr)
        menu->close();
    if (custom == nullptr) {
        std::cerr << "label colour: no Custom… entry\n";
        ++gFindings;
        return;
    }
    custom->trigger();
    settle(200);
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && widget->objectName() == QStringLiteral("kColorPickerPopup")) {
            shoot(QStringLiteral("label-colour-picker"), *widget);
            widget->close();
            return;
        }
    std::cerr << "label colour: the picker did not open\n";
    ++gFindings;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bloom_dialog_capture <directory> <1|1.5>\n";
        return 2;
    }
    // The offscreen platform's default 800 x 600 screen would clamp a tall dialog (and the
    // 1920 x 1200 main-window fixture); give it a desktop-sized screen.
    QTemporaryDir platformDirectory;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && platformDirectory.isValid()) {
        const auto config = QDir(platformDirectory.path()).filePath(QStringLiteral("screens.json"));
        QFile file(config);
        if (file.open(QIODevice::WriteOnly))
            file.write(R"({"screens":[{"name":"capture","x":0,"y":0,"width":3840,"height":2400,)"
                       R"("logicalDpi":96,"logicalBaseDpi":96,"dpr":1}]})");
        file.close();
        qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + config).toLocal8Bit());
    }
    qputenv("QT_SCALE_FACTOR", argv[2]);
    qputenv("BLOOM_REDUCED_MOTION", "1");
    // The captured file dialogs save their sidebar and history into the Qt settings: keep them
    // out of the user's own.
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    gSuffix = QString::fromLatin1(argv[2]).remove(QLatin1Char('.'));
    QDir().mkpath(gDirectory);
    QTemporaryDir scratch;
    test::WindowFixture fixture;
    fixture.window->show();
    fixture.projectHost.setDialogOwner(fixture.window.get());
    fixture.exporter->setDialogOwner(fixture.window.get());
    settle(500);
    compositionDialogs(fixture);
    exportDialogs(fixture);
    projectDialogs(fixture);
    importDialogs(fixture);
    fileChoosers(fixture, scratch.path());
    labelColourPicker(fixture);
    assetDialogs(fixture, scratch.path());
    compositionDeleteAndViewer(fixture);
    std::cout << "findings: " << gFindings << '\n';
    return gFindings;
}
