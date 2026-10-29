// The Viewer chrome's shared helpers, the footer's state updates, and the
// no-composition invitation. The rows themselves are built in viewer_editor_chrome.cpp.
#include "composition_editor_support.hpp"
#include "viewer_editor_internal.hpp"
#include "viewer_timecode_readout.hpp"

#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color.hpp>
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

namespace viewer_detail {
std::optional<color::ResolvedBloomNeutralConfig>
resolveViewerColorConfig(const CompositionSession& session) {
    const auto* builtIn = std::get_if<document::BuiltInOcioConfigLocator>(
        &session.colorSettings().ocioConfig.locator);
    if (builtIn == nullptr) {
        return std::nullopt;
    }
    auto result =
        color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
                                  session.colorSettings().ocioConfig.expectedRevision.digest,
                                  session.colorIntent().workingColorSpaceId);
    return std::move(result).takeResolved();
}

bool hasLookTaggedEffect(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return false;
    }
    for (const auto& node : composition->graph().nodes()) {
        if (node.typeId != "bloom.ocio-file-transform") {
            continue;
        }
        const auto look =
            std::ranges::find(node.parameters, "look", &document::ParameterBinding::role);
        if (look == node.parameters.end()) {
            continue;
        }
        const auto* record = composition->parameters().find(look->parameterId);
        if (record == nullptr) {
            continue;
        }
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        if (constant == nullptr) {
            return true;
        }
        const auto* enabled = std::get_if<bool>(&constant->value);
        if (enabled == nullptr || *enabled) {
            return true;
        }
    }
    return false;
}

QString safeAreaPresetName(const ViewerSafeAreaPreset preset) {
    switch (preset) {
    case ViewerSafeAreaPreset::Broadcast:
        return QStringLiteral("Broadcast");
    case ViewerSafeAreaPreset::Hd:
        return QStringLiteral("HD");
    case ViewerSafeAreaPreset::Cinema:
        return QStringLiteral("Cinema");
    case ViewerSafeAreaPreset::Social:
        return QStringLiteral("Social");
    case ViewerSafeAreaPreset::Custom:
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Broadcast");
}

ViewerSafeAreaPreset safeAreaPresetFromName(const QString& name) {
    if (name == QStringLiteral("HD"))
        return ViewerSafeAreaPreset::Hd;
    if (name == QStringLiteral("Cinema"))
        return ViewerSafeAreaPreset::Cinema;
    if (name == QStringLiteral("Social"))
        return ViewerSafeAreaPreset::Social;
    if (name == QStringLiteral("Custom"))
        return ViewerSafeAreaPreset::Custom;
    return ViewerSafeAreaPreset::Broadcast;
}

QString safeAreaPresetSetting(const document::CompositionId id) {
    return QStringLiteral("viewer/overlay/safe-area-preset/%1").arg(id.value());
}

QString viewerResolutionText(const CompositionPreviewController& controller) {
    const auto policy = controller.settings().resolutionPolicy;
    const auto divisor = controller.resolutionDivisor();
    const QString factor = divisor == 4   ? QStringLiteral("¼")
                           : divisor == 2 ? QStringLiteral("½")
                                          : QStringLiteral("1");
    return policy == runtime::PreviewResolutionPolicy::Auto
               ? ViewerEditor::tr("Auto · %1").arg(factor)
               : ViewerEditor::tr(kResolutionNames[static_cast<std::size_t>(policy)]);
}

QToolButton* makeFooterButton(const kit::IconId iconId, const QString& toolTip,
                              const QString& accessibleName, const QString& objectName,
                              QWidget* parent) {
    auto* button = new kit::KIconButton(parent);
    button->setObjectName(objectName);
    button->setIcon(kit::icon(iconId, kit::IconRole::Control));
    const int box = kit::px(kit::iconSize(kit::IconRole::Control));
    button->setIconSize(QSize(box, box));
    button->setToolTip(toolTip);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    button->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    return button;
}

ViewerBackground viewerBackgroundFor(const ViewerBackgroundPreference preference) noexcept {
    switch (preference) {
    case ViewerBackgroundPreference::Grey:
        return ViewerBackground::Grey;
    case ViewerBackgroundPreference::Black:
        return ViewerBackground::Black;
    case ViewerBackgroundPreference::White:
        return ViewerBackground::White;
    case ViewerBackgroundPreference::Solid:
        break;
    }
    return ViewerBackground::Solid;
}

QColor viewerSurroundColor(const ViewerBackground background) {
    switch (background) {
    case ViewerBackground::Grey:
        return kit::color(kit::Color::CanvasGrey);
    case ViewerBackground::Black:
        return kit::color(kit::Color::CanvasBlack);
    case ViewerBackground::White:
        return kit::color(kit::Color::CanvasWhite);
    case ViewerBackground::Solid:
        break;
    }
    return kit::color(kit::Color::Canvas);
}

QColor compositionFrameColor(const document::Composition* const composition) {
    if (composition == nullptr)
        return kit::color(kit::Color::Canvas);
    const auto authored = composition->backgroundColor();
    // The same quantization the Composition Settings chip and its hex label use, so the frame
    // shows exactly the colour the chip shows. A background is opaque in the Viewer (as in After
    // Effects); its alpha is never presented and never fills export alpha.
    return kit::KColor{static_cast<float>(authored.red), static_cast<float>(authored.green),
                       static_cast<float>(authored.blue), 1.0F}
        .toQColor();
}

void drawCanvasBackground(QPainter& painter, const QRectF& bounds,
                          const ViewerBackground background) {
    painter.fillRect(bounds, viewerSurroundColor(background));
}

void drawFrameBackground(QPainter& painter, const QRectF& content, const QRectF& frame,
                         const QColor& frameColor, const bool transparencyGrid) {
    const QRectF visible = frame.intersected(content);
    if (visible.isEmpty())
        return;
    if (!transparencyGrid) {
        painter.fillRect(visible, frameColor);
        return;
    }
    // The Viewer's checker, laid on the content area's grid (the GPU present anchors its checker
    // at the container origin too) and clipped to the composition frame.
    painter.save();
    painter.setClipRect(visible, Qt::IntersectClip);
    kit::drawAlphaCheckerboard(painter, content, kit::px(kit::Size::ViewerChecker),
                               kit::Color::ViewerCheckerLight, kit::Color::ViewerCheckerDark);
    painter.restore();
}
} // namespace viewer_detail

namespace viewer_detail {
kit::KValueField* makeAdjustField(const QString& label, const QString& accessibleName,
                                  const QString& objectName, const double minimum,
                                  const double maximum, const double step, QWidget* parent) {
    auto* field = new kit::KValueField(parent);
    field->setObjectName(objectName);
    field->setLabel(label);
    field->setAccessibleName(accessibleName);
    field->setCompact(true);
    // A field in a chrome row keeps its resting outline.
    field->setOutlinedAtRest(true);
    field->setRange(minimum, maximum);
    field->setSingleStep(step);
    field->setDecimals(2);
    field->setFixedWidth(kit::px(kit::Size::ViewerAdjustWidth));
    return field;
}

void attachDefaultReset(kit::KValueField& field, const double defaultValue, QString resetLabel,
                        QAction* resetBoth) {
    // The kit field's own default: the differs-from-default dot, the double-click on its label and
    // Reset in its context menu, which also lists the shared Reset Exposure and Gamma.
    field.setDefaultValue(defaultValue);
    field.setResetText(std::move(resetLabel));
    if (resetBoth != nullptr)
        field.addAction(resetBoth);
}
} // namespace viewer_detail

// ---- Footer state ------------------------------------------------------------------------------

void ViewerEditor::setResolutionCompact(const bool compact) {
    for (std::size_t index = 0; index < kResolutionNames.size(); ++index)
        resolutionDropdown_->setItemText(
            static_cast<int>(index),
            tr(compact ? kResolutionCompactNames[index] : kResolutionNames[index]));
}

void ViewerEditor::resetViewAdjust() {
    exposureField_->cancelEdit();
    gammaField_->cancelEdit();
    exposureField_->setValue(0.0);
    gammaField_->setValue(1.0);
}

void ViewerEditor::updateRoiControls() {
    if (roiClearButton_ == nullptr)
        return;
    const bool exists = roiRect_.has_value();
    clearRoiAction_->setEnabled(exists);
    // Clear ROI shows only while there is a region to clear.
    roiClearButton_->setProperty("chromeSuppressed", !exists);
    if (!exists && statusBarFooter_ == nullptr)
        roiClearButton_->hide();
}

void ViewerEditor::updatePlaybackButton(const PlaybackState state) {
    const bool playing = state == PlaybackState::Playing;
    playPauseButton_->setChecked(playing);
    playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
    playPauseButton_->setIcon(
        kit::icon(playing ? kit::IconId::Pause : kit::IconId::Play, kit::IconRole::Control));
    // RAM Preview folded into the play button's CACHED STATE (task VIEW-1): the button says
    // whether the range it is about to play is already in the cache.
    const bool cached = ramPreview_ != nullptr && !ramPreview_->isCaching() &&
                        ramPreview_->totalFrameCount() > 0 &&
                        ramPreview_->cachedFrameCount() >= ramPreview_->totalFrameCount();
    if (playing) {
        playPauseButton_->setToolTip(tr("Pause playback (Space)"));
    } else if (cached) {
        playPauseButton_->setToolTip(tr("Play cached range (Space)"));
    } else {
        playPauseButton_->setToolTip(tr("Play (Space)"));
    }
    playPauseButton_->setProperty("bloomPlaybackCached", cached);
}

void ViewerEditor::updateRamPreviewButton() {
    if (ramPreview_ == nullptr) {
        return;
    }
    const bool caching = ramPreview_->isCaching();
    ramPreviewButton_->setChecked(caching);
    ramPreviewAction_->setChecked(caching);
    // The label is the progress: how much of the range is cached while it caches, and that it is
    // all cached once it is. Both counts are the controller's own.
    const auto total = ramPreview_->totalFrameCount();
    const auto cachedFrames = ramPreview_->cachedFrameCount();
    QString label = tr("RAM Preview");
    if (caching && total > 0)
        label = tr("Caching %1%").arg(std::min<std::uint64_t>(100, cachedFrames * 100 / total));
    else if (!caching && total > 0 && cachedFrames >= total)
        label = tr("RAM Cached");
    ramPreviewButton_->setText(label);
    ramPreviewButton_->setToolTip(
        caching ? tr("Cancel the RAM preview being cached (Esc)")
                : tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"));
    if (!caching &&
        ramPreview_->lastCompletion() == RamPreviewController::Completion::PartialBudget) {
        ramPreviewButton_->setToolTip(
            tr("RAM Preview stopped at the memory limit: %1 of %2 frames retained.\nLongest cached "
               "span: %3 frames. Use Play for manual playback.")
                .arg(ramPreview_->cachedFrameCount())
                .arg(ramPreview_->totalFrameCount())
                .arg(ramPreview_->playableSpan() ? ramPreview_->playableSpan()->frameCount : 0));
    }
}

void ViewerEditor::updateLoopButton() {
    const bool looping = playback_->isLooping();
    loopButton_->setChecked(looping);
    loopButton_->setToolTip(looping ? tr("Looping: playback wraps to the start of the work area")
                                    : tr("Not looping: playback stops on the last frame"));
    loopButton_->setAccessibleName(looping ? tr("Looping on") : tr("Looping off"));
}

void ViewerEditor::refreshZoomDropdown() {
    updatePreviewResolution();
    if (resolutionReadout_ != nullptr) {
        resolutionReadout_->setText(viewerResolutionText(previewController_));
        resolutionDropdown_->setToolTip(
            tr("Resolution: %1").arg(viewerResolutionText(previewController_)));
    }
    if (zoomDropdown_ == nullptr) {
        return;
    }
    int targetIndex = 0; // "Fit"
    if (!transform_.fitToWindow) {
        const int percent = static_cast<int>(std::lround(transform_.zoom * 100.0));
        int presetIndex = -1;
        for (std::size_t i = 0; i < kZoomPresets.size(); ++i) {
            if (kZoomPresets[i] == percent) {
                presetIndex = static_cast<int>(i) + 1;
                break;
            }
        }
        if (presetIndex >= 0) {
            targetIndex = presetIndex;
        } else {
            // A zoom off the fixed ladder (e.g. from a wheel step) gets one trailing "current
            // custom value" item (decision 3), renamed in place on every later custom zoom.
            const QString label = tr("%1%").arg(percent);
            if (zoomDropdown_->count() > kFixedZoomItemCount) {
                zoomDropdown_->setItemText(kFixedZoomItemCount, label);
            } else {
                zoomDropdown_->addItem(label, percent);
            }
            targetIndex = kFixedZoomItemCount;
        }
    }
    if (zoomDropdown_->currentIndex() != targetIndex) {
        const QSignalBlocker blocker(zoomDropdown_);
        zoomDropdown_->setCurrentIndex(targetIndex);
    }
    zoomDropdown_->update();
}

// ---- The no-composition invitation -------------------------------------------------------------

void ViewerEditor::buildEmptyState() {
    emptyState_ = new QWidget(this);
    emptyState_->setObjectName(QStringLiteral("viewerEmptyState"));
    emptyState_->setFixedWidth(kit::px(kit::Size::ViewerEmptyStateWidth));
    auto* column = new QVBoxLayout(emptyState_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(kit::px(kit::Spacing::S));
    auto* title = new kit::KLabel(tr("No composition"), emptyState_, kit::TypeRole::Title);
    title->setObjectName(QStringLiteral("viewerEmptyStateTitle"));
    title->setAlignment(Qt::AlignHCenter);
    auto* hint = new kit::KLabel(
        tr("Create a composition to begin. Media files dropped here are imported into Assets."),
        emptyState_);
    hint->setObjectName(QStringLiteral("viewerEmptyStateHint"));
    hint->setTone(kit::Color::Muted);
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignHCenter);
    auto* button = new kit::KButton(tr("New Composition…"), emptyState_);
    button->setObjectName(QStringLiteral("viewerNewCompositionButton"));
    button->setVariant(kit::KButton::Variant::Primary);
    button->setToolTip(tr("New Composition… (Ctrl+Alt+N)"));
    // The same command as Composition > New Composition…: the application's own action when the
    // Viewer is hosted, the Viewer's retired copy of it otherwise.
    connect(button, &QAbstractButton::clicked, this, [this] {
        if (!triggerApplicationCommand(QStringLiteral("compositionNewAction")))
            viewerCompositionNewAction_->trigger();
    });
    column->addWidget(title);
    column->addWidget(hint);
    column->addWidget(button, 0, Qt::AlignHCenter);
    updateEmptyState();
}

void ViewerEditor::layoutEmptyState() {
    if (emptyState_ == nullptr || emptyState_->isHidden())
        return;
    emptyState_->adjustSize();
    const QRectF frame = canvasRect();
    const QSize size = emptyState_->size();
    emptyState_->move(static_cast<int>(std::lround(frame.center().x() - size.width() / 2.0)),
                      static_cast<int>(std::lround(frame.center().y() - size.height() / 2.0)));
}

void ViewerEditor::updateEmptyState() {
    if (emptyState_ == nullptr)
        return;
    emptyState_->setVisible(session_.composition() == nullptr);
    layoutEmptyState();
}

} // namespace bloom::ui
