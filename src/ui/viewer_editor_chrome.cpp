// The Viewer's chrome: the header selectors and the footer's three groups with their priority
// collapse. Their shared helpers, the footer's state updates, the tool column and the
// no-composition invitation are in viewer_editor_controls.cpp. The menus they share actions with
// are in viewer_editor_menus.cpp; docs/ux/ui-grammar.md ("Viewer header and footer") is the
// specification.
#include "composition_editor_support.hpp"
#include "viewer_editor_internal.hpp"
#include "viewer_timecode_readout.hpp"

#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace bloom::ui {
using namespace viewer_detail;

// ---- Header ------------------------------------------------------------------------------------

void ViewerEditor::buildHeader() {
    auto* bar = &chrome_.header;
    bar->owner = this;
    bar->objectName = "viewerHeaderMenuBar";
    bar->overflowButtonName = "viewerHeaderOverflowButton";
    bar->overflowMenuName = "viewerHeaderOverflowMenu";

    // The two selectors name what they select: the composition the Viewer shows, and the layer
    // the panels act on. Empty, each says so and is disabled; narrow, each elides (the tooltip
    // keeps the full name) before any menu collapses.
    compositionSelector_ = new kit::KDropdown(this);
    compositionSelector_->setObjectName(QStringLiteral("viewerCompositionSelector"));
    compositionSelector_->setAccessibleName(tr("Composition"));
    compositionSelector_->setMaximumWidth(kit::px(kit::Size::DropdownWidthExpanded));
    connect(compositionSelector_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0) {
                    return;
                }
                const auto id = document::CompositionId::fromRaw(
                    compositionSelector_->itemData(index).toULongLong());
                if (id.isValid()) {
                    (void)session_.setComposition(id);
                }
            });
    bar->addWidget(compositionSelector_);
    bar->setElideFloor(compositionSelector_, kit::px(kit::Size::DropdownWidth));

    // Retired: the composition commands live in the application's Composition menu. The hidden
    // button keeps the old automation identities, and each of its actions runs the application's
    // own command when the Viewer is hosted in a window that has one.
    compositionMenuButton_ = new kit::KIconButton(this);
    compositionMenuButton_->setObjectName(QStringLiteral("viewerCompositionMenuButton"));
    compositionMenuButton_->setIcon(kit::icon(kit::IconId::ContextMenu, kit::IconRole::Chrome));
    compositionMenuButton_->setToolTip(tr("Composition commands"));
    compositionMenuButton_->setAccessibleName(tr("Composition commands"));
    auto* compositionMenu = kit::makeMenu(this);
    compositionMenu->setObjectName(QStringLiteral("viewerCompositionMenu"));
    compositionMenuButton_->setMenu(compositionMenu);
    compositionMenuButton_->setPopupMode(QToolButton::InstantPopup);
    compositionMenuButton_->hide();
    const auto addCompositionAction = [this, compositionMenu](const QString& text,
                                                              const QString& objectName,
                                                              const QString& command,
                                                              std::function<void()> fallback) {
        auto* action = compositionMenu->addAction(text);
        action->setObjectName(objectName);
        connect(action, &QAction::triggered, this,
                [this, command, fallback = std::move(fallback)] {
                    if (!triggerApplicationCommand(command))
                        fallback();
                });
        return action;
    };
    viewerCompositionNewAction_ = addCompositionAction(
        tr("New Composition…"), QStringLiteral("viewerNewCompositionAction"),
        QStringLiteral("compositionNewAction"), [this] {
            if (const auto id = showNewCompositionDialog(session_, *this); id.has_value())
                (void)session_.setComposition(*id);
        });
    viewerCompositionDuplicateAction_ = addCompositionAction(
        tr("Duplicate Composition"), QStringLiteral("viewerDuplicateCompositionAction"),
        QStringLiteral("compositionDuplicateAction"), [this] {
            if (const auto id = duplicateComposition(session_, session_.compositionId()))
                (void)session_.setComposition(*id);
        });
    viewerCompositionSettingsAction_ = addCompositionAction(
        tr("Composition Settings…"), QStringLiteral("viewerCompositionSettingsAction"),
        QStringLiteral("compositionSettingsAction"),
        [this] { (void)showCompositionSettingsDialog(session_, session_.compositionId(), *this); });
    viewerCompositionRenameAction_ = addCompositionAction(
        tr("Rename Composition…"), QStringLiteral("viewerRenameCompositionAction"),
        QStringLiteral("compositionRenameAction"),
        [this] { renameComposition(session_, session_.compositionId(), *this); });
    viewerCompositionDeleteAction_ = addCompositionAction(
        tr("Delete Composition"), QStringLiteral("viewerDeleteCompositionAction"),
        QStringLiteral("compositionDeleteAction"),
        [this] { requestDeleteComposition(session_, session_.compositionId(), *this); });

    objectSelector_ = new kit::KDropdown(this);
    objectSelector_->setObjectName(QStringLiteral("viewerObjectSelector"));
    objectSelector_->setAccessibleName(tr("Selected layer"));
    objectSelector_->setMaximumWidth(kit::px(kit::Size::DropdownWidthExpanded));
    connect(objectSelector_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (index <= 0) {
            session_.clearSelection();
            return;
        }
        const auto id = document::LayerId::fromRaw(objectSelector_->itemData(index).toULongLong());
        if (id.isValid()) {
            session_.selectLayer(id);
        }
    });
    bar->addWidget(objectSelector_);
    bar->setElideFloor(objectSelector_, kit::px(kit::Size::DropdownWidth));

    bar->addMenuButton(tr("View"), viewerViewMenu_, QStringLiteral("viewerViewMenuButton"));
    bar->addMenuButton(tr("Select"), viewerSelectMenu_, QStringLiteral("viewerSelectMenuButton"));
    bar->addMenuButton(tr("Add"), viewerAddMenu_, QStringLiteral("viewerAddMenuButton"));
    bar->addStretch();

    fullscreenButton_ = new kit::KIconButton(this);
    fullscreenButton_->setObjectName(QStringLiteral("viewerFullscreenButton"));
    fullscreenButton_->setIcon(kit::icon(kit::IconId::Maximize, kit::IconRole::Chrome));
    fullscreenButton_->setToolTip(tr("Full Screen (F11)"));
    fullscreenButton_->setAccessibleName(tr("Full Screen (F11)"));
    fullscreenButton_->setCheckable(true);
    fullscreenButton_->setChecked(window() != nullptr && window()->isFullScreen());
    connect(fullscreenButton_, &QToolButton::clicked, this, [this] {
        if (triggerApplicationCommand(QStringLiteral("viewFullScreenAction")))
            fullscreenButton_->setChecked(window()->isFullScreen());
    });
    fullscreenButton_->hide();
    headerMenuWidget_ = EditorArea::buildChromeRow(chrome_.header, this);
    headerMenuWidget_->hide(); // The canvas remains full-bleed until EditorArea hosts chrome.

    rebuildCompositionSelector();
    rebuildObjectSelector();
    updateCompositionActions();
}

bool ViewerEditor::triggerApplicationCommand(const QString& objectName) {
    auto* host = window();
    if (host == nullptr || host == this)
        return false;
    auto* action = host->findChild<QAction*>(objectName);
    auto* owner = action != nullptr ? qobject_cast<QWidget*>(action->parent()) : nullptr;
    if (action == nullptr || owner == this || (owner != nullptr && isAncestorOf(owner)))
        return false;
    action->trigger();
    return true;
}

void ViewerEditor::rebuildCompositionSelector() {
    if (compositionSelector_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(compositionSelector_);
    compositionSelector_->clearItems();
    const auto compositions = session_.snapshot().project().compositions();
    const bool empty = compositions.empty();
    compositionSelector_->setEnabled(!empty);
    compositionSelector_->setMutedValue(empty);
    if (empty) {
        const int index = compositionSelector_->addItem(tr("No composition"));
        compositionSelector_->setItemToolTip(
            index, tr("Composition shown in the Viewer: this project has no composition yet"));
        return;
    }
    int selectedIndex = 0;
    int index = 0;
    for (const auto& composition : compositions) {
        const auto name = QString::fromStdString(composition.name());
        compositionSelector_->addItem(name,
                                      QVariant::fromValue<qulonglong>(composition.id().value()));
        compositionSelector_->setItemToolTip(index,
                                             tr("Composition shown in the Viewer: %1").arg(name));
        if (composition.id() == session_.compositionId()) {
            selectedIndex = index;
        }
        ++index;
    }
    // Choosing an item also makes its tooltip the selector's own.
    compositionSelector_->setCurrentIndex(selectedIndex);
}

void ViewerEditor::rebuildObjectSelector() {
    if (objectSelector_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(objectSelector_);
    objectSelector_->clearItems();
    const auto* composition = session_.composition();
    const bool hasLayers = composition != nullptr && !composition->graph().layerOutputs().empty();
    // Index 0 is always the "nothing selected" placeholder, so choosing it clears the selection.
    const int placeholder = objectSelector_->addItem(
        hasLayers ? tr("No layer selected") : tr("No layers"), QVariant::fromValue<qulonglong>(0));
    objectSelector_->setItemToolTip(
        placeholder, composition == nullptr
                         ? tr("Selected layer: there is no composition to choose a layer from")
                     : hasLayers ? tr("Selected layer: choose a layer to select it")
                                 : tr("Selected layer: this composition has no layers yet"));
    objectSelector_->setEnabled(hasLayers);
    if (!hasLayers) {
        objectSelector_->setMutedValue(true);
        return;
    }
    const auto selectedLayer = session_.selection().contextualLayer;
    int selectedIndex = 0;
    int index = 1;
    for (const auto& boundary : composition->graph().layerOutputs()) {
        const auto name = layerName(*composition, boundary.layerId);
        objectSelector_->addItem(name, QVariant::fromValue<qulonglong>(boundary.layerId.value()));
        objectSelector_->setItemToolTip(index, tr("Selected layer: %1").arg(name));
        if (selectedLayer.has_value() && *selectedLayer == boundary.layerId) {
            selectedIndex = index;
        }
        ++index;
    }
    objectSelector_->setCurrentIndex(selectedIndex);
    objectSelector_->setMutedValue(selectedIndex == 0);
}

void ViewerEditor::updateCompositionActions() {
    if (viewerCompositionNewAction_ == nullptr) {
        return;
    }
    const bool hasComposition = session_.composition() != nullptr;
    viewerCompositionDuplicateAction_->setEnabled(hasComposition);
    viewerCompositionRenameAction_->setEnabled(hasComposition);
    viewerCompositionSettingsAction_->setEnabled(hasComposition);
    // Deleting the final composition is allowed and returns to the blank, usable empty project.
    viewerCompositionDeleteAction_->setEnabled(hasComposition);
}

// ---- Footer ------------------------------------------------------------------------------------

void ViewerEditor::rebuildDisplayViewControl() {
    if (viewerDisplayView_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(viewerDisplayView_);
    viewerDisplayView_->clearItems();
    const auto resolved = resolveViewerColorConfig(session_);
    if (!resolved.has_value() || resolved->displays().empty()) {
        viewerDisplayView_->addItem(tr("Unavailable"));
        viewerDisplayView_->setItemToolTip(
            0, tr("Display / View: the colour configuration offers no display"));
        viewerDisplayView_->setCurrentIndex(0);
        viewerDisplayView_->setEnabled(false);
        viewerDisplayView_->setMutedValue(true);
        return;
    }

    // The closed field names the view alone ("sRGB", "ACES 1.0 SDR"), which is what an artist
    // reads; a view several displays share is qualified by its display. The full display, view
    // and colour space are in the tooltip and in the menus.
    std::map<std::string, int> viewUses;
    for (const auto& entry : resolved->displays())
        ++viewUses[entry.view];
    int defaultIndex = 0;
    for (std::size_t index = 0; index < resolved->displays().size(); ++index) {
        const auto& entry = resolved->displays()[index];
        const auto display = QString::fromStdString(entry.display);
        const auto view = QString::fromStdString(entry.view);
        const auto full = QStringLiteral("%1 / %2").arg(display, view);
        const auto data = QStringList{display, view};
        // A human name ("sRGB"), never the config's identifier; the raw names are in the tooltip.
        const QString viewLabel = displayViewText(view, view);
        const int itemIndex = viewerDisplayView_->addItem(
            viewUses[entry.view] > 1 ? displayViewText(display, view) : viewLabel, data);
        viewerDisplayView_->setItemToolTip(
            itemIndex, tr("Display / View: %1 · %2 (viewer display only, never export)")
                           .arg(full, QString::fromStdString(entry.colourSpaceId)));
        if (entry.isDefault) {
            defaultIndex = itemIndex;
        }
    }
    // Typical names fit whole; a longer one elides, its tooltip and the menus keep it entire.
    viewerDisplayView_->setFixedWidth(
        std::clamp(viewerDisplayView_->sizeHint().width(), kit::px(kit::Size::DropdownWidth),
                   kit::px(kit::Size::ViewerDisplayViewWidth)));
    const auto saved = QSettings().value(displayViewSettingsKey()).toStringList();
    int selectedIndex = defaultIndex;
    if (saved.size() == 2) {
        for (int index = 0; index < viewerDisplayView_->count(); ++index) {
            if (viewerDisplayView_->itemData(index).toStringList() == saved) {
                selectedIndex = index;
                break;
            }
        }
    }
    viewerDisplayView_->setEnabled(true);
    viewerDisplayView_->setMutedValue(false);
    viewerDisplayView_->setCurrentIndex(selectedIndex);
    const auto selected = viewerDisplayView_->itemData(selectedIndex).toStringList();
    if (selected.size() == 2) {
        previewController_.setViewerDisplayView(selected[0].toStdString(),
                                                selected[1].toStdString());
    }
}

void ViewerEditor::updateLookControl() {
    if (lookAction_ == nullptr) {
        return;
    }
    const bool available = hasLookTaggedEffect(session_);
    lookAction_->setEnabled(available);
    lookAction_->setToolTip(
        available ? tr("Look: include look-tagged effects in the viewer")
                  : tr("Look unavailable: this composition has no look-tagged effect"));
}

void ViewerEditor::buildTransportActions() {
    // Frame-stepping shortcuts (issue #108, decisions 1/2), mirroring playPauseAction's own
    // WindowShortcut idiom exactly. Created with the footer so a collapsed Go to Start/End can be
    // offered in its overflow menu; wireTransport() connects them.
    const auto makeStepAction = [this](const QString& text, const QString& objectName,
                                       const QKeySequence& shortcut) {
        auto* action = new QAction(text, this);
        action->setObjectName(objectName);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        return action;
    };
    stepBackwardAction_ =
        makeStepAction(tr("Step Back One Frame"), QStringLiteral("stepBackwardAction"),
                       QKeySequence(Qt::Key_Left));
    stepForwardAction_ =
        makeStepAction(tr("Step Forward One Frame"), QStringLiteral("stepForwardAction"),
                       QKeySequence(Qt::Key_Right));
    stepToStartAction_ = makeStepAction(tr("Go To Start"), QStringLiteral("stepToStartAction"),
                                        QKeySequence(Qt::Key_Home));
    stepToEndAction_ = makeStepAction(tr("Go To End"), QStringLiteral("stepToEndAction"),
                                      QKeySequence(Qt::Key_End));
}

void ViewerEditor::buildFooter(RamPreviewController* const ramPreview) {
    auto* footer = this;
    auto& row = chrome_.footer;
    row.objectName = "viewerFooter";
    row.overflowButtonName = "viewerFooterOverflowButton";
    row.overflowMenuName = "viewerFooterOverflowMenu";
    row.overflowIcon = kit::IconId::SlidersHorizontal;
    row.overflowToolTip = tr("View options");

    // ---- View and display (the leading group) -------------------------------------------------
    channelDropdown_ = new kit::KDropdown(footer);
    channelDropdown_->setObjectName("viewerChannelDropdown");
    channelDropdown_->setAccessibleName(tr("Channel"));
    channelDropdown_->setToolTip(tr("Channel: which channels the viewer shows; never an export"));
    for (const auto* name : kChannelNames) {
        const int index = channelDropdown_->addItem(tr(name));
        channelDropdown_->setItemToolTip(
            index, tr("Channel: %1 (viewer display only, never export)").arg(tr(name)));
    }
    channelDropdown_->setFixedWidth(kit::px(kit::Size::ViewerChannelWidth));
    connect(channelDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) { chooseChannelDropdownIndex(index); });

    // The Transparency Grid (After Effects' Toggle Transparency Grid): one action behind the
    // footer toggle, View and the overflow menu. Its checked state is restored with the surround in
    // the Background block below, before anything can present.
    transparencyGridAction_ = new QAction(tr("Transparency Grid"), this);
    transparencyGridAction_->setObjectName(QStringLiteral("viewerTransparencyGridAction"));
    transparencyGridAction_->setCheckable(true);
    transparencyGridAction_->setIcon(
        kit::icon(kit::IconId::TransparencyGrid, kit::IconRole::Control));
    transparencyGridAction_->setToolTip(
        tr("Transparency grid: show a checkerboard instead of the composition background "
           "colour; viewer display only"));
    connect(transparencyGridAction_, &QAction::toggled, this,
            [this](const bool enabled) { setTransparencyGrid(enabled); });
    transparencyGridButton_ = makeFooterButton(
        kit::IconId::TransparencyGrid, transparencyGridAction_->toolTip(),
        tr("Transparency grid"), QStringLiteral("viewerTransparencyGridToggle"), footer);
    transparencyGridButton_->setDefaultAction(transparencyGridAction_);

    // ROI: one action behind the footer toggle, View and the overflow menu. Clear ROI is offered
    // only while a region exists.
    roiAction_ = new QAction(tr("Region of Interest"), this);
    roiAction_->setObjectName(QStringLiteral("viewerRoiAction"));
    roiAction_->setCheckable(true);
    roiAction_->setIcon(kit::icon(kit::IconId::RegionOfInterest, kit::IconRole::Control));
    roiAction_->setToolTip(tr("Region of interest: evaluate only this region (Ctrl-drag with "
                              "Select draws it)"));
    connect(roiAction_, &QAction::toggled, this, [this] { publishRoi(); });
    roiButton_ = makeFooterButton(kit::IconId::RegionOfInterest, roiAction_->toolTip(),
                                  tr("Region of interest"), QStringLiteral("viewerRoiToggle"),
                                  footer);
    roiButton_->setDefaultAction(roiAction_);
    clearRoiAction_ = new QAction(tr("Clear Region of Interest"), this);
    clearRoiAction_->setObjectName(QStringLiteral("viewerClearRoiAction"));
    clearRoiAction_->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Control));
    clearRoiAction_->setToolTip(tr("Clear region of interest"));
    connect(clearRoiAction_, &QAction::triggered, this, [this] {
        roiGesture_.reset();
        roiRect_.reset();
        roiAction_->setChecked(false);
        publishRoi();
    });
    roiClearButton_ = makeFooterButton(kit::IconId::Close, tr("Clear region of interest"),
                                       tr("Clear region of interest"),
                                       QStringLiteral("viewerRoiClear"), footer);
    roiClearButton_->setDefaultAction(clearRoiAction_);

    resetViewAdjustAction_ = new QAction(tr("Reset Exposure and Gamma"), this);
    resetViewAdjustAction_->setObjectName(QStringLiteral("viewerResetViewAdjustAction"));
    resetViewAdjustAction_->setIcon(kit::icon(kit::IconId::Reset, kit::IconRole::Chrome));
    connect(resetViewAdjustAction_, &QAction::triggered, this, &ViewerEditor::resetViewAdjust);
    exposureField_ = makeAdjustField(tr("EV"), tr("Viewer exposure in EV stops"),
                                     QStringLiteral("viewerExposure"), -32, 32, 0.1, footer);
    exposureField_->setToolTip(
        tr("Exposure in EV stops; viewer display only. Double-click EV to reset"));
    gammaField_ = makeAdjustField(tr("γ"), tr("Viewer gamma"), QStringLiteral("viewerGamma"), 0.01,
                                  10, 0.05, footer);
    gammaField_->setValue(1);
    gammaField_->setToolTip(
        tr("Gamma after display encoding; viewer display only. Double-click γ to reset"));
    attachDefaultReset(*exposureField_, 0.0, tr("Reset Exposure"), resetViewAdjustAction_);
    attachDefaultReset(*gammaField_, 1.0, tr("Reset Gamma"), resetViewAdjustAction_);
    const auto changed = [this] {
        viewAdjust_ = {exposureField_->value(), gammaField_->value()};
        const auto prefix = analysisSettingsPrefix();
        QSettings settings;
        settings.setValue(prefix + "exposure", viewAdjust_.exposure);
        settings.setValue(prefix + "gamma", viewAdjust_.gamma);
        resetViewAdjustAction_->setEnabled(viewAdjust_.exposure != 0.0 ||
                                           viewAdjust_.gamma != 1.0);
        if (overflowExposureField_ != nullptr) {
            const QSignalBlocker exposure(overflowExposureField_);
            const QSignalBlocker gamma(overflowGammaField_);
            overflowExposureField_->setValue(viewAdjust_.exposure);
            overflowGammaField_->setValue(viewAdjust_.gamma);
        }
        refreshViewAdjustment();
        update();
    };
    connect(exposureField_, &kit::KValueField::valueChanged, this, changed);
    connect(gammaField_, &kit::KValueField::valueChanged, this, changed);

    // The overflow menu's exposure and gamma: labelled copies bound to the same two values.
    {
        auto* panel = new QWidget(footer);
        panel->setObjectName(QStringLiteral("viewerOverflowViewAdjust"));
        auto* form = new QFormLayout(panel);
        const int inset = kit::px(kit::Spacing::MenuItemX);
        form->setContentsMargins(inset, kit::px(kit::Spacing::XS), inset,
                                 kit::px(kit::Spacing::XS));
        form->setHorizontalSpacing(kit::px(kit::Spacing::S));
        form->setVerticalSpacing(kit::px(kit::Spacing::XS));
        overflowExposureField_ = makeAdjustField({}, tr("Viewer exposure in EV stops"),
                                                 QStringLiteral("viewerOverflowExposure"), -32, 32,
                                                 0.1, panel);
        overflowGammaField_ = makeAdjustField({}, tr("Viewer gamma"),
                                              QStringLiteral("viewerOverflowGamma"), 0.01, 10, 0.05,
                                              panel);
        overflowGammaField_->setValue(1);
        form->addRow(new kit::KLabel(tr("Exposure (EV)"), panel), overflowExposureField_);
        form->addRow(new kit::KLabel(tr("Gamma"), panel), overflowGammaField_);
        connect(overflowExposureField_, &kit::KValueField::valueChanged, exposureField_,
                &kit::KValueField::setValue);
        connect(overflowGammaField_, &kit::KValueField::valueChanged, gammaField_,
                &kit::KValueField::setValue);
        viewAdjustOverflowAction_ = new QWidgetAction(this);
        viewAdjustOverflowAction_->setObjectName(QStringLiteral("viewerViewAdjustOverflowAction"));
        viewAdjustOverflowAction_->setText(tr("Exposure and Gamma"));
        viewAdjustOverflowAction_->setDefaultWidget(panel);
    }
    loadViewAdjust();

    viewerDisplayView_ = new kit::KDropdown(footer);
    viewerDisplayView_->setObjectName("viewerDisplayView");
    viewerDisplayView_->setAccessibleName(tr("Display and view"));
    connect(viewerDisplayView_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0) {
                    return;
                }
                const auto selected = viewerDisplayView_->itemData(index).toStringList();
                if (selected.size() != 2) {
                    return;
                }
                QSettings().setValue(displayViewSettingsKey(), selected);
                previewController_.setViewerDisplayView(selected[0].toStdString(),
                                                        selected[1].toStdString());
            });

    lookAction_ = new QAction(tr("Look"), this);
    lookAction_->setObjectName(QStringLiteral("viewerLookAction"));
    lookAction_->setCheckable(true);
    lookAction_->setIcon(kit::icon(kit::IconId::Visible, kit::IconRole::Control));
    viewerLookToggle_ = makeFooterButton(kit::IconId::Visible, tr("Look"), tr("Look"),
                                         QStringLiteral("viewerLookToggle"), footer);
    viewerLookToggle_->setDefaultAction(lookAction_);
    const bool showLook = QSettings().value(lookSettingsKey(), true).toBool();
    lookAction_->setChecked(showLook);
    previewController_.setViewerLookEnabled(showLook);
    connect(lookAction_, &QAction::toggled, this, [this](const bool enabled) {
        QSettings().setValue(lookSettingsKey(), enabled);
        previewController_.setViewerLookEnabled(enabled);
    });
    rebuildDisplayViewControl();
    updateLookControl();

    // ---- Framing and quality (the trailing group) ----------------------------------------------
    zoomDropdown_ = new kit::KDropdown(footer);
    zoomDropdown_->setObjectName("viewerZoomDropdown");
    zoomDropdown_->setAccessibleName(tr("Zoom"));
    zoomDropdown_->setToolTip(tr("Zoom: Fit (Ctrl+0), 100% (Ctrl+1), or a preset"));
    zoomDropdown_->addItem(tr("Fit"), 0);
    for (const int percent : kZoomPresets) {
        zoomDropdown_->addItem(tr("%1%").arg(percent), percent);
    }
    zoomDropdown_->setWidthFloor(kit::px(kit::Size::ViewerZoomWidth));
    connect(zoomDropdown_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (index <= 0) {
            setZoomFit();
            return;
        }
        setZoomPercent(zoomDropdown_->itemData(index).toInt());
    });

    resolutionDropdown_ = new kit::KDropdown(footer);
    resolutionDropdown_->setObjectName("viewerResolutionDropdown");
    resolutionDropdown_->setAccessibleName(tr("Resolution"));
    for (const auto* name : kResolutionNames) {
        resolutionDropdown_->addItem(tr(name), QString::fromLatin1(name));
    }
    resolutionDropdown_->setFixedWidth(kit::px(kit::Size::ViewerResolutionWidth));
    const auto saved = QSettings().value(kResolutionSetting, QStringLiteral("Auto")).toString();
    int savedIndex = 0;
    for (std::size_t i = 0; i < kResolutionNames.size(); ++i) {
        if (saved == QLatin1StringView(kResolutionNames[i])) {
            savedIndex = static_cast<int>(i);
        }
    }
    resolutionDropdown_->setCurrentIndex(savedIndex);
    previewController_.setResolutionPolicy(
        static_cast<runtime::PreviewResolutionPolicy>(savedIndex));
    connect(resolutionDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0 || index >= static_cast<int>(kResolutionNames.size())) {
                    return;
                }
                QSettings().setValue(
                    kResolutionSetting,
                    QString::fromLatin1(kResolutionNames[static_cast<std::size_t>(index)]));
                previewController_.setResolutionPolicy(
                    static_cast<runtime::PreviewResolutionPolicy>(index));
            });
    connect(&previewController_, &CompositionPreviewController::resolutionChanged, this, [this] {
        const QSignalBlocker blocker(resolutionDropdown_);
        resolutionDropdown_->setCurrentIndex(
            static_cast<int>(previewController_.settings().resolutionPolicy));
        resolutionReadout_->setText(viewerResolutionText(previewController_));
        resolutionDropdown_->setToolTip(tr("Resolution: %1").arg(viewerResolutionText(previewController_)));
    });
    // Retired as a footer item, kept for automation: what the chosen policy resolved to. The
    // Resolution tooltip says the same.
    resolutionReadout_ = new kit::KLabel(footer);
    resolutionReadout_->setObjectName("viewerResolutionReadout");
    resolutionReadout_->setAccessibleName(tr("Effective preview resolution"));
    resolutionReadout_->setFont(kit::font(kit::TypeRole::Value));
    resolutionReadout_->setText(viewerResolutionText(previewController_));
    resolutionReadout_->hide();
    resolutionDropdown_->setToolTip(tr("Resolution: %1").arg(viewerResolutionText(previewController_)));

    // Retired as a footer item, kept for automation and the preferences path: the surround is
    // chosen from View > Background. The stored value goes through the Preferences model's one
    // migration rule, so a pre-split "Checkerboard" reads as Solid with the Transparency Grid on.
    backgroundDropdown_ = new kit::KDropdown(footer);
    backgroundDropdown_->setObjectName("viewerBackgroundDropdown");
    backgroundDropdown_->setAccessibleName(tr("Background"));
    for (const auto* name : kBackgroundNames) {
        backgroundDropdown_->addItem(tr(name));
    }
    const auto backdrop = viewerBackdropFromSettings(QSettings());
    background_ = viewerBackgroundFor(backdrop.background);
    transparencyGrid_ = backdrop.transparencyGrid;
    backgroundDropdown_->setCurrentIndex(static_cast<int>(background_));
    connect(backgroundDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](const int index) {
                if (index < 0 || index >= static_cast<int>(kBackgroundNames.size())) {
                    return;
                }
                setBackground(static_cast<ViewerBackground>(index));
            });
    backgroundDropdown_->hide();
    {
        const QSignalBlocker blocker(transparencyGridAction_);
        transparencyGridAction_->setChecked(transparencyGrid_);
    }

    // RAM Preview: a labelled toggle, because caching a range and playing it are different
    // commands; its label carries the caching progress (updateRamPreviewButton()).
    ramPreviewButton_ = new kit::KButton(kit::IconId::Sequence, tr("RAM Preview"), footer);
    ramPreviewButton_->setObjectName(QStringLiteral("timelineRamPreviewButton"));
    ramPreviewButton_->setAccessibleName(tr("RAM preview"));
    ramPreviewButton_->setToolTip(
        tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"));
    ramPreviewButton_->setCheckable(true);
    ramPreviewButton_->setEnabled(ramPreview != nullptr);
    // Wide enough for every label it can show, so the progress never moves the footer.
    int ramWidth = kit::px(kit::Size::ViewerModeWidth);
    for (const auto& label : {tr("RAM Preview"), tr("Caching 100%"), tr("RAM Cached")}) {
        ramPreviewButton_->setText(label);
        ramWidth = std::max(ramWidth, ramPreviewButton_->sizeHint().width());
    }
    ramPreviewButton_->setText(tr("RAM Preview"));
    ramPreviewButton_->setFixedWidth(ramWidth);
    // The overflow menu's entry for it. The shortcut is the Composition menu's (one owner per
    // sequence); the entry only shows it.
    ramPreviewAction_ = new QAction(tr("RAM Preview") + QStringLiteral("\tCtrl+Shift+Space"), this);
    ramPreviewAction_->setObjectName(QStringLiteral("viewerRamPreviewAction"));
    ramPreviewAction_->setIcon(kit::icon(kit::IconId::Sequence, kit::IconRole::Chrome));
    ramPreviewAction_->setCheckable(true);
    ramPreviewAction_->setEnabled(ramPreview != nullptr);

    // ---- Time (the centre group) --------------------------------------------------------------
    // Moved here from the timeline (task VIEW-1). Every objectName below is unchanged: these
    // controls changed parent, not identity.
    stepToStartButton_ =
        makeFooterButton(kit::IconId::CaretLeft, tr("Go to start (Home)"), tr("Go to start"),
                         QStringLiteral("viewerStepToStartButton"), footer);
    stepBackButton_ = makeFooterButton(kit::IconId::StepBack, tr("Step back one frame (Left)"),
                                       tr("Step back one frame"),
                                       QStringLiteral("timelineStepBackButton"), footer);
    // playPauseButton_ MUST stay a QToolButton with its existing text()/isChecked() contract
    // (playback_controller_tests.cpp, composition_projection_test.cpp both read it by exactly that
    // type/objectName).
    playPauseButton_ =
        makeFooterButton(kit::IconId::Play, tr("Toggle playback (Space)"), tr("Toggle playback"),
                         QStringLiteral("playPauseButton"), footer);
    playPauseButton_->setCheckable(true);
    stepForwardButton_ = makeFooterButton(
        kit::IconId::StepForward, tr("Step forward one frame (Right)"),
        tr("Step forward one frame"), QStringLiteral("timelineStepForwardButton"), footer);
    stepToEndButton_ =
        makeFooterButton(kit::IconId::CaretRight, tr("Go to end (End)"), tr("Go to end"),
                         QStringLiteral("viewerStepToEndButton"), footer);
    // The loop control is a real toggle, with PlaybackController::setLooping() behind it. Same
    // objectName as the timeline glyph it replaced, and the same Selected plate as every other
    // footer toggle.
    loopButton_ = makeFooterButton(kit::IconId::Loop, tr("Loop playback"), tr("Loop playback"),
                                   QStringLiteral("timelineLoopIndicator"), footer);
    loopButton_->setCheckable(true);
    timeReadout_ = new ViewerTimecodeReadout(
        session_, [this](const std::uint64_t frameIndex) { seekToFrame(frameIndex); }, footer);

    buildTransportActions();
    buildMenus();

    // ---- The row -------------------------------------------------------------------------------
    for (auto* control : std::initializer_list<QWidget*>{
             channelDropdown_, transparencyGridButton_, roiButton_, roiClearButton_,
             exposureField_, gammaField_,
             viewerDisplayView_, viewerLookToggle_})
        row.addWidget(control);
    row.beginCentre();
    for (auto* control : std::initializer_list<QWidget*>{
             stepToStartButton_, stepBackButton_, playPauseButton_, stepForwardButton_,
             stepToEndButton_, loopButton_, timeReadout_})
        row.addWidget(control);
    row.addStretch();
    for (auto* control :
         std::initializer_list<QWidget*>{zoomDropdown_, resolutionDropdown_, ramPreviewButton_})
        row.addWidget(control);
    row.setCollapse(viewerLookToggle_, CollapseLook, lookAction_);
    row.setCollapse(viewerDisplayView_, CollapseDisplayView, displayViewMenu_->menuAction());
    row.setCollapse(exposureField_, CollapseViewAdjust, viewAdjustOverflowAction_);
    row.setCollapse(gammaField_, CollapseViewAdjust, resetViewAdjustAction_);
    row.setCollapse(roiButton_, CollapseRoi, roiAction_);
    row.setCollapse(roiClearButton_, CollapseRoi, clearRoiAction_);
    row.setCollapse(transparencyGridButton_, CollapseTransparencyGrid, transparencyGridAction_);
    row.setCollapse(channelDropdown_, CollapseChannel, channelMenu_->menuAction());
    row.setCompactForm(resolutionDropdown_, CompactResolution,
                       kit::px(kit::Size::ViewerResolutionCompactWidth),
                       [this](const bool compact) { setResolutionCompact(compact); });
    row.setCollapse(ramPreviewButton_, CollapseRamPreview, ramPreviewAction_);
    row.setCollapse(stepToStartButton_, CollapseStepToEnds, stepToStartAction_);
    row.setCollapse(stepToEndButton_, CollapseStepToEnds, stepToEndAction_);
    row.setCollapse(zoomDropdown_, CollapseFraming, zoomMenu_->menuAction());
    row.setCollapse(resolutionDropdown_, CollapseFraming, resolutionMenu_->menuAction());
    row.setCompactForm(timeReadout_, CompactFrameField, ViewerTimecodeReadout::compactWidth(),
                       [this](const bool compact) { timeReadout_->setCompact(compact); });
    updateRoiControls();
    statusBarFooter_ = EditorArea::buildChromeRow(row, this, true);
    chrome_.hosted = [this] {
        statusBarFooterTaken_ = true;
        loadViewAdjust();
        updatePreviewResolution();
        update();
    };
    layoutStatusBar();
}

} // namespace bloom::ui
