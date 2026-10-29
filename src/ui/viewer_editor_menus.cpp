// The Viewer's View, Select and Add menus, the canvas context menu, and the guide state they
// drive. One action object per command, shared by the menus, the footer (and its overflow menu)
// and the context menu, so a checked state can never disagree between them. The structure is
// specified in docs/ux/ui-grammar.md ("Viewer menus").
#include "composition_editor_support.hpp"
#include "layer_drop.hpp"
#include "viewer_editor_internal.hpp"

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>

namespace bloom::ui {
using namespace viewer_detail;

namespace {
// A menu that offers a dropdown's items, item for item, under `label(index)`, with the current
// one checked. The dropdown stays the one source of truth: choosing a menu item chooses the
// dropdown item, and the menu re-reads the dropdown whenever it opens or the choice changes.
void syncMirror(QMenu& menu, const kit::KDropdown& dropdown, const QString& objectPrefix,
                const std::function<QString(int)>& label) {
    const int count = dropdown.count();
    auto actions = menu.actions();
    bool same = actions.size() == count;
    for (int index = 0; same && index < count; ++index)
        same = actions[index]->text() == label(index);
    if (!same) {
        menu.clear();
        for (auto* group : menu.findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly))
            delete group;
        auto* group = new QActionGroup(&menu);
        group->setExclusive(true);
        for (int index = 0; index < count; ++index) {
            auto* action = menu.addAction(label(index));
            action->setObjectName(QStringLiteral("%1%2Action").arg(objectPrefix).arg(index));
            action->setCheckable(true);
            action->setData(index);
            group->addAction(action);
        }
        actions = menu.actions();
    }
    for (int index = 0; index < count; ++index) {
        actions[index]->setEnabled(dropdown.isItemEnabled(index));
        actions[index]->setChecked(index == dropdown.currentIndex());
    }
    menu.menuAction()->setEnabled(dropdown.isEnabled() && count > 0);
}

QMenu* mirrorMenu(const QString& title, const QString& objectName, kit::KDropdown* dropdown,
                  const QString& objectPrefix, std::function<QString(int)> label,
                  QWidget* owner) {
    auto* menu = kit::makeMenu(title, owner);
    menu->setObjectName(objectName);
    const auto sync = [menu, dropdown, objectPrefix, label] {
        syncMirror(*menu, *dropdown, objectPrefix, label);
    };
    QObject::connect(menu, &QMenu::aboutToShow, owner, sync);
    QObject::connect(dropdown, &kit::KDropdown::currentIndexChanged, owner, sync);
    QObject::connect(menu, &QMenu::triggered, owner, [menu, dropdown](QAction* action) {
        if (menu->actions().contains(action))
            dropdown->setCurrentIndex(action->data().toInt());
    });
    sync();
    return menu;
}
} // namespace

void ViewerEditor::buildMenus() {
    // ---- View ----------------------------------------------------------------------------------
    viewerViewMenu_ = kit::makeMenu(tr("View"), this);
    viewerViewMenu_->setObjectName(QStringLiteral("viewerViewMenu"));
    const auto shortcutAction = [this](const QString& text, const QString& objectName,
                                       const QKeySequence& shortcut) {
        auto* action = new QAction(text, this);
        action->setObjectName(objectName);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        return action;
    };
    viewerFitAction_ = shortcutAction(tr("Fit"), QStringLiteral("viewerFitAction"),
                                      QKeySequence(Qt::CTRL | Qt::Key_0));
    // "100%", the name the zoom presets use, everywhere this command appears.
    viewerActualSizeAction_ = shortcutAction(tr("100%"), QStringLiteral("viewerActualSizeAction"),
                                             QKeySequence(Qt::CTRL | Qt::Key_1));
    viewerZoomInAction_ = shortcutAction(tr("Zoom In"), QStringLiteral("viewerZoomInAction"),
                                         QKeySequence(Qt::CTRL | Qt::Key_Plus));
    viewerZoomOutAction_ = shortcutAction(tr("Zoom Out"), QStringLiteral("viewerZoomOutAction"),
                                          QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(viewerFitAction_, &QAction::triggered, this, &ViewerEditor::setZoomFit);
    connect(viewerActualSizeAction_, &QAction::triggered, this, &ViewerEditor::setZoomActualSize);
    connect(viewerZoomInAction_, &QAction::triggered, this, &ViewerEditor::zoomInAtCenter);
    connect(viewerZoomOutAction_, &QAction::triggered, this, &ViewerEditor::zoomOutAtCenter);
    for (auto* action :
         {viewerFitAction_, viewerActualSizeAction_, viewerZoomInAction_, viewerZoomOutAction_})
        viewerViewMenu_->addAction(action);
    zoomMenu_ = mirrorMenu(
        tr("Zoom"), QStringLiteral("viewerZoomMenu"), zoomDropdown_,
        QStringLiteral("viewerZoomPreset"),
        [this](const int index) { return zoomDropdown_->itemText(index); }, this);
    viewerViewMenu_->addMenu(zoomMenu_);
    resolutionMenu_ = mirrorMenu(
        tr("Resolution"), QStringLiteral("viewerResolutionMenu"), resolutionDropdown_,
        QStringLiteral("viewerResolution"),
        [](const int index) {
            return index >= 0 && index < static_cast<int>(kResolutionNames.size())
                       ? tr(kResolutionNames[static_cast<std::size_t>(index)])
                       : QString{};
        },
        this);
    viewerViewMenu_->addMenu(resolutionMenu_);
    viewerViewMenu_->addSeparator();

    channelMenu_ = mirrorMenu(
        tr("Channel"), QStringLiteral("viewerChannelMenu"), channelDropdown_,
        QStringLiteral("viewerChannel"),
        [this](const int index) { return channelDropdown_->itemText(index); }, this);
    viewerViewMenu_->addMenu(channelMenu_);
    displayViewMenu_ = mirrorMenu(
        tr("Display / View"), QStringLiteral("viewerDisplayViewMenu"), viewerDisplayView_,
        QStringLiteral("viewerDisplayView"),
        [this](const int index) {
            // The full display and view, which the compact footer field leaves to its tooltip.
            const auto pair = viewerDisplayView_->itemData(index).toStringList();
            return pair.size() == 2 ? QStringLiteral("%1 / %2").arg(pair[0], pair[1])
                                    : viewerDisplayView_->itemText(index);
        },
        this);
    viewerViewMenu_->addMenu(displayViewMenu_);
    viewerViewMenu_->addAction(lookAction_);
    viewerViewMenu_->addAction(resetViewAdjustAction_);
    viewerViewMenu_->addAction(roiAction_);
    viewerViewMenu_->addAction(clearRoiAction_);
    viewerViewMenu_->addSeparator();

    // ---- View > Guides -------------------------------------------------------------------------
    guidesMenu_ = kit::makeMenu(tr("Guides"), this);
    guidesMenu_->setObjectName(QStringLiteral("viewerGuidesMenu"));
    safeAreasAction_ = shortcutAction(tr("Safe Areas"), QStringLiteral("viewerSafeAreasAction"),
                                      QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
    safeAreasAction_->setCheckable(true);
    overlayOptions_.safeAreas = QSettings().value(kSafeAreasSetting, false).toBool();
    safeAreasAction_->setChecked(overlayOptions_.safeAreas);
    connect(safeAreasAction_, &QAction::toggled, this, [this](const bool enabled) {
        overlayOptions_.safeAreas = enabled;
        QSettings().setValue(kSafeAreasSetting, enabled);
        ++gpuOverlayRevision_;
        updateGpuResidentPresentation();
        update();
    });
    guidesMenu_->addAction(safeAreasAction_);

    safeAreaPresetMenu_ = guidesMenu_->addMenu(tr("Safe Area Preset"));
    safeAreaPresetMenu_->setObjectName(QStringLiteral("viewerSafeAreaPresetMenu"));
    const std::array<std::pair<QString, ViewerSafeAreaPreset>, 5> presets{{
        {tr("Broadcast (4:3)"), ViewerSafeAreaPreset::Broadcast},
        {tr("HD (16:9)"), ViewerSafeAreaPreset::Hd},
        {tr("Cinema"), ViewerSafeAreaPreset::Cinema},
        {tr("Social"), ViewerSafeAreaPreset::Social},
        {tr("Custom…"), ViewerSafeAreaPreset::Custom},
    }};
    for (std::size_t index = 0; index < presets.size(); ++index) {
        auto* action = safeAreaPresetMenu_->addAction(presets[index].first);
        action->setObjectName(QStringLiteral("viewerSafeArea%1Action").arg(index));
        action->setCheckable(true);
        safeAreaPresetActions_[index] = action;
        connect(action, &QAction::triggered, this,
                [this, preset = presets[index].second] { applySafeAreaPreset(preset); });
    }
    auto* customSafeAreas = guidesMenu_->addAction(tr("Custom Safe Areas…"));
    customSafeAreas->setObjectName(QStringLiteral("viewerCustomSafeAreasAction"));
    connect(customSafeAreas, &QAction::triggered, this, &ViewerEditor::showCustomSafeAreaDialog);
    connect(guidesMenu_, &QMenu::aboutToShow, this, [this, customSafeAreas] {
        customSafeAreas->setEnabled(session_.composition() != nullptr);
    });
    guidesMenu_->addSeparator();

    const auto addOverlayToggle = [this, &shortcutAction](const QString& label,
                                                          const QString& objectName,
                                                          const QString& setting,
                                                          const QKeySequence& shortcut,
                                                          bool* state) {
        auto* action = shortcutAction(label, objectName, shortcut);
        action->setCheckable(true);
        *state = QSettings().value(setting, false).toBool();
        action->setChecked(*state);
        guidesMenu_->addAction(action);
        connect(action, &QAction::toggled, this, [state, setting](const bool enabled) {
            *state = enabled;
            QSettings().setValue(setting, enabled);
        });
        connect(action, &QAction::toggled, this, [this] {
            ++gpuOverlayRevision_;
            updateGpuResidentPresentation();
            update();
        });
        return action;
    };
    centreCrossAction_ = addOverlayToggle(
        tr("Centre Cross"), QStringLiteral("viewerCentreCrossAction"), kCentreCrossSetting,
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), &overlayOptions_.centreCross);
    thirdsAction_ =
        addOverlayToggle(tr("Thirds"), QStringLiteral("viewerThirdsAction"), kThirdsSetting,
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), &overlayOptions_.thirds);
    rulersAction_ =
        addOverlayToggle(tr("Rulers"), QStringLiteral("viewerRulers"), kRulersSetting,
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), &overlayOptions_.rulers);
    pixelGridAction_ = addOverlayToggle(
        tr("Pixel Grid"), QStringLiteral("viewerPixelGridAction"), kPixelGridSetting,
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), &overlayOptions_.pixelGrid);
    viewerViewMenu_->addMenu(guidesMenu_);
    backgroundMenu_ = mirrorMenu(
        tr("Background"), QStringLiteral("viewerBackgroundMenu"), backgroundDropdown_,
        QStringLiteral("viewerBackground"),
        [this](const int index) { return backgroundDropdown_->itemText(index); }, this);
    viewerViewMenu_->addMenu(backgroundMenu_);

    // ---- Select --------------------------------------------------------------------------------
    viewerSelectMenu_ = kit::makeMenu(tr("Select"), this);
    viewerSelectMenu_->setObjectName(QStringLiteral("viewerSelectMenu"));
    selectAllAction_ = shortcutAction(tr("Select All Layers"),
                                      QStringLiteral("viewerSelectAllAction"),
                                      QKeySequence(Qt::CTRL | Qt::Key_A));
    selectNoneAction_ = shortcutAction(tr("Deselect All Layers"),
                                       QStringLiteral("viewerSelectNoneAction"),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    invertSelectionAction_ = shortcutAction(tr("Invert Layer Selection"),
                                            QStringLiteral("viewerInvertSelectionAction"),
                                            QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
    for (auto* action : {selectAllAction_, selectNoneAction_, invertSelectionAction_})
        viewerSelectMenu_->addAction(action);
    connect(selectAllAction_, &QAction::triggered, this, &ViewerEditor::selectAllObjects);
    connect(selectNoneAction_, &QAction::triggered, this, &ViewerEditor::selectNoObjects);
    connect(invertSelectionAction_, &QAction::triggered, this,
            &ViewerEditor::invertObjectSelection);
    connect(viewerSelectMenu_, &QMenu::aboutToShow, this, &ViewerEditor::updateSelectActions);

    // ---- Add -----------------------------------------------------------------------------------
    viewerAddMenu_ = kit::makeMenu(tr("Add"), this);
    viewerAddMenu_->setObjectName("viewerAddMenu");
    auto* solid = viewerAddMenu_->addAction(tr("Solid"));
    solid->setObjectName(QStringLiteral("viewerAddSolidAction"));
    connect(solid, &QAction::triggered, this, [this] { (void)addDefaultSolidLayer(session_); });
    auto* text = viewerAddMenu_->addAction(tr("Text"));
    text->setObjectName(QStringLiteral("viewerAddTextAction"));
    connect(text, &QAction::triggered, this, [this] { (void)addDefaultTextLayer(session_); });
    viewerAddMenu_->addSeparator();
    for (std::int64_t index = 0; index <= 6; ++index) {
        const auto kind = static_cast<document::ShapeKind>(index);
        auto* shapeAction =
            viewerAddMenu_->addAction(QString::fromUtf8(document::shapeKindName(kind).data()));
        shapeAction->setObjectName(QStringLiteral("viewerAddShape.%1").arg(index));
        connect(shapeAction, &QAction::triggered, this,
                [this, kind] { (void)addDefaultShapeLayer(session_, kind); });
    }
    viewerAddMenu_->addSeparator();
    auto* importAndAdd = viewerAddMenu_->addAction(tr("Import and Add…"));
    importAndAdd->setObjectName(QStringLiteral("viewerImportAndAddAction"));
    importAndAdd->setToolTip(tr("Import media into Assets, then add it as layers at the playhead"));
    connect(importAndAdd, &QAction::triggered, this, &ViewerEditor::importAndAddLayers);
    connect(viewerAddMenu_, &QMenu::aboutToShow, this, [this] {
        const bool hasComposition = session_.composition() != nullptr;
        for (auto* action : viewerAddMenu_->actions())
            if (!action->isSeparator())
                action->setEnabled(hasComposition);
        if (auto* import = viewerAddMenu_->findChild<QAction*>(
                QStringLiteral("viewerImportAndAddAction")))
            import->setEnabled(hasComposition && session_.assetController() != nullptr);
    });

    // The footer's RAM Preview entry runs the same application-wide command as its button.
    if (ramPreview_ != nullptr)
        connect(ramPreviewAction_, &QAction::triggered, this, [this] {
            ramPreviewButton_->click();
        });
}

void ViewerEditor::importAndAddLayers() {
    auto* controller = session_.assetController();
    if (controller == nullptr || session_.composition() == nullptr)
        return;
    // The drop path's own follow-up: the import commits first, then one undoable step adds layers
    // for what it imported, on top, at the playhead.
    controller->requestImport(
        this, importedLayersFollowUp(session_, session_.compositionId(),
                                     LayerDropPlacement{.start = session_.currentTime()}));
}

// ---- Select ------------------------------------------------------------------------------------

bool ViewerEditor::selectsAnchors() const {
    const auto target = editablePath();
    if (!target || target->path.anchors.empty())
        return false;
    return tool_ == Tool::Pen || target->owner == EditablePath::Owner::Mask ||
           !anchorSelectionFor(*target).empty();
}

void ViewerEditor::updateSelectActions() {
    const bool anchors = selectsAnchors();
    selectAllAction_->setText(anchors ? tr("Select All Anchors") : tr("Select All Layers"));
    selectNoneAction_->setText(anchors ? tr("Deselect All Anchors") : tr("Deselect All Layers"));
    invertSelectionAction_->setText(anchors ? tr("Invert Anchor Selection")
                                            : tr("Invert Layer Selection"));
    const auto* composition = session_.composition();
    const bool any = anchors || (composition != nullptr &&
                                 !composition->graph().layerOutputs().empty());
    for (auto* action : {selectAllAction_, selectNoneAction_, invertSelectionAction_})
        action->setEnabled(any);
}

void ViewerEditor::selectAllObjects() {
    if (selectsAnchors()) {
        const auto target = editablePath();
        path_editing::AnchorSet all;
        for (std::size_t index = 0; index < target->path.anchors.size(); ++index)
            all.insert(index);
        setAnchorSelection(*target, std::move(all));
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    std::set<document::NodeId> nodes;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        if (boundary.nodeId.isValid()) {
            nodes.insert(boundary.nodeId);
        }
    }
    if (!nodes.empty()) {
        session_.selectNodes(nodes, *nodes.begin());
    }
}

void ViewerEditor::selectNoObjects() {
    if (selectsAnchors()) {
        setAnchorSelection(*editablePath(), {});
        return;
    }
    session_.clearSelection();
}

void ViewerEditor::invertObjectSelection() {
    if (selectsAnchors()) {
        const auto target = editablePath();
        const auto selected = anchorSelectionFor(*target);
        path_editing::AnchorSet inverted;
        for (std::size_t index = 0; index < target->path.anchors.size(); ++index)
            if (!selected.contains(index))
                inverted.insert(index);
        setAnchorSelection(*target, std::move(inverted));
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    std::set<document::NodeId> all;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        if (boundary.nodeId.isValid()) {
            all.insert(boundary.nodeId);
        }
    }
    std::set<document::NodeId> remaining;
    std::ranges::set_difference(all, session_.selectedNodes(),
                                std::inserter(remaining, remaining.end()));
    if (remaining.empty()) {
        session_.clearSelection();
    } else {
        session_.selectNodes(remaining, *remaining.begin());
    }
}

// ---- Guides ------------------------------------------------------------------------------------

void ViewerEditor::updateOverlayActions() {
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        overlayOptions_.safeAreaSettings = composition->safeAreas();
        const QString saved =
            QSettings().value(safeAreaPresetSetting(composition->id())).toString();
        if (!saved.isEmpty()) {
            overlayOptions_.safeAreaPreset = safeAreaPresetFromName(saved);
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.93) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.90) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Hd;
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.90) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.85) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Cinema;
        } else if (std::abs(overlayOptions_.safeAreaSettings.action - 0.90) < 0.0001 &&
                   std::abs(overlayOptions_.safeAreaSettings.title - 0.80) < 0.0001) {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Broadcast;
        } else {
            overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Custom;
        }
    } else {
        overlayOptions_.safeAreaSettings = {};
        overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Broadcast;
    }
    if (safeAreasAction_ != nullptr) {
        const QSignalBlocker blocker(safeAreasAction_);
        safeAreasAction_->setChecked(overlayOptions_.safeAreas);
        safeAreasAction_->setEnabled(composition != nullptr);
    }
    if (safeAreaPresetMenu_ != nullptr) {
        safeAreaPresetMenu_->setEnabled(composition != nullptr);
        for (std::size_t index = 0; index < safeAreaPresetActions_.size(); ++index) {
            const QSignalBlocker blocker(safeAreaPresetActions_[index]);
            safeAreaPresetActions_[index]->setChecked(
                static_cast<int>(overlayOptions_.safeAreaPreset) == static_cast<int>(index));
        }
    }
}

void ViewerEditor::applySafeAreaPreset(const ViewerSafeAreaPreset preset) {
    if (preset == ViewerSafeAreaPreset::Custom) {
        showCustomSafeAreaDialog();
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    document::SafeAreaSettings settings{};
    switch (preset) {
    case ViewerSafeAreaPreset::Broadcast:
        settings = {.action = 0.90, .title = 0.80};
        break;
    case ViewerSafeAreaPreset::Hd:
        settings = {.action = 0.93, .title = 0.90};
        break;
    case ViewerSafeAreaPreset::Cinema:
        settings = {.action = 0.90, .title = 0.85};
        break;
    case ViewerSafeAreaPreset::Social:
        settings = {.action = 0.90, .title = 0.80};
        break;
    case ViewerSafeAreaPreset::Custom:
        return;
    }
    commands::Transaction transaction(QStringLiteral("Set Safe Area Preset").toStdString(),
                                      session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionSafeAreas>(composition->id(), settings);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        overlayOptions_.safeAreaPreset = preset;
        QSettings().setValue(safeAreaPresetSetting(composition->id()), safeAreaPresetName(preset));
        updateOverlayActions();
        update();
    }
}

void ViewerEditor::showCustomSafeAreaDialog() {
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("viewerSafeAreaDialog"));
    dialog.setWindowTitle(tr("Custom Safe Areas"));
    auto* form = new QFormLayout(&dialog);
    auto* action = new QDoubleSpinBox(&dialog);
    action->setObjectName(QStringLiteral("viewerActionSafeAreaField"));
    action->setRange(1.0, 100.0);
    action->setDecimals(1);
    action->setSuffix(QStringLiteral("%"));
    action->setValue(composition->safeAreas().action * 100.0);
    form->addRow(tr("Action"), action);
    auto* title = new QDoubleSpinBox(&dialog);
    title->setObjectName(QStringLiteral("viewerTitleSafeAreaField"));
    title->setRange(1.0, 100.0);
    title->setDecimals(1);
    title->setSuffix(QStringLiteral("%"));
    title->setValue(composition->safeAreas().title * 100.0);
    form->addRow(tr("Title"), title);
    auto* error = new kit::KLabel(&dialog);
    error->setObjectName(QStringLiteral("viewerSafeAreaErrorLabel"));
    error->hide();
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("viewerSafeAreaDialogButtons"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, action, title, error] {
        if (title->value() > action->value()) {
            error->setText(QObject::tr("Title safe area must not exceed Action safe area."));
            error->show();
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const document::SafeAreaSettings settings{action->value() / 100.0, title->value() / 100.0};
    commands::Transaction transaction(QStringLiteral("Set Custom Safe Areas").toStdString(),
                                      session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionSafeAreas>(composition->id(), settings);
    if (session_.executeTransaction(std::move(transaction)).succeeded()) {
        overlayOptions_.safeAreaPreset = ViewerSafeAreaPreset::Custom;
        QSettings().setValue(safeAreaPresetSetting(composition->id()),
                             safeAreaPresetName(ViewerSafeAreaPreset::Custom));
        updateOverlayActions();
        update();
    }
}

// ---- Canvas context menu -----------------------------------------------------------------------

std::unique_ptr<QMenu> ViewerEditor::canvasContextMenu(const QPointF point) {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("viewerCanvasContextMenu"));
    // The layer under the pointer first, as in After Effects: select it, or duplicate or delete it
    // through the same commands as the Timeline's layer menu.
    const auto* composition = session_.composition();
    if (const auto mapping = currentMapping(); mapping && composition != nullptr) {
        const auto hit = hitAt(*mapping, point);
        const auto node = hit.region != ViewerHitRegion::Empty
                              ? session_.boundaryNodeForLayer(hit.layerId)
                              : std::nullopt;
        if (node.has_value()) {
            const auto layerId = hit.layerId;
            const auto compositionId = session_.compositionId();
            auto* select = menu->addAction(tr("Select %1").arg(layerName(*composition, layerId)));
            select->setObjectName(QStringLiteral("viewerContextSelectLayerAction"));
            connect(select, &QAction::triggered, this,
                    [this, layerId] { session_.selectLayer(layerId); });
            auto* duplicate = menu->addAction(tr("Duplicate Layer"));
            duplicate->setObjectName(QStringLiteral("viewerContextDuplicateLayerAction"));
            connect(duplicate, &QAction::triggered, this, [this, compositionId, node] {
                commands::Transaction transaction("Duplicate Layers",
                                                  session_.snapshot().revision());
                transaction.emplace<commands::DuplicateNodes>(
                    compositionId, std::set<document::NodeId>{*node}, document::Vec2d{0, 0});
                (void)session_.executeTransaction(std::move(transaction));
            });
            auto* remove = menu->addAction(tr("Delete Layer"));
            remove->setObjectName(QStringLiteral("viewerContextDeleteLayerAction"));
            connect(remove, &QAction::triggered, this, [this, compositionId, node] {
                commands::Transaction transaction("Delete Layers", session_.snapshot().revision());
                transaction.emplace<commands::RemoveNodes>(compositionId,
                                                           std::set<document::NodeId>{*node});
                (void)session_.executeTransaction(std::move(transaction));
            });
            menu->addSeparator();
        }
    }
    const bool canZoom = currentDisplayGeometry().has_value();
    viewerZoomInAction_->setEnabled(canZoom);
    viewerZoomOutAction_->setEnabled(canZoom);
    for (auto* action :
         {viewerFitAction_, viewerActualSizeAction_, viewerZoomInAction_, viewerZoomOutAction_})
        menu->addAction(action);
    menu->addSeparator();
    emit viewerAddMenu_->aboutToShow();
    menu->addMenu(viewerAddMenu_);
    emit guidesMenu_->aboutToShow();
    menu->addMenu(guidesMenu_);
    if (composition != nullptr) {
        menu->addSeparator();
        auto* settings = menu->addAction(tr("Composition Settings…"));
        settings->setObjectName(QStringLiteral("viewerContextCompositionSettingsAction"));
        connect(settings, &QAction::triggered, this, [this] {
            if (!triggerApplicationCommand(QStringLiteral("compositionSettingsAction")))
                viewerCompositionSettingsAction_->trigger();
        });
    }
    return menu;
}

std::unique_ptr<QMenu> ViewerEditor::canvasContextMenuForTest(const QPointF point) {
    return canvasContextMenu(point);
}

void ViewerEditor::contextMenuEvent(QContextMenuEvent* event) {
    // The anchor menu comes first while a path is being edited.
    if (anchorContextMenuEvent(event))
        return;
    auto menu = canvasContextMenu(event->pos());
    menu->exec(event->globalPos());
    event->accept();
}

} // namespace bloom::ui
