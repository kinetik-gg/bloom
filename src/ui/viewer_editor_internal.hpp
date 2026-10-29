#pragma once
// Private to the Viewer's own translation units (viewer_editor.cpp, viewer_editor_chrome.cpp,
// viewer_editor_menus.cpp, viewer_timecode_readout.cpp): the vocabularies and helpers the
// canvas, its chrome and its menus share. Never included outside src/ui.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QColor>
#include <QString>

#include <array>
#include <optional>
#include <string>
#include <string_view>

class QAction;
class QPainter;
class QRectF;
class QToolButton;
class QWidget;

namespace bloom::ui::kit {
class KValueField;
}
namespace bloom::document {
class Composition;
}

namespace bloom::ui {
class CompositionPreviewController;
class CompositionSession;

namespace viewer_detail {
// The Fit item plus the fixed percentage presets a real gesture never removes (decision 3). A
// zoom that lands off this ladder (e.g. from a wheel step) gets ONE additional trailing item; see
// ViewerEditor::refreshZoomDropdown().
// The display and view an artist reads: each name through the colour registry's labels, and one
// name alone when both read the same (Bloom Neutral's single "sRGB" display and view). The raw OCIO
// names stay in the item data and the tooltip.
[[nodiscard]] inline QString displayViewText(const QString& display, const QString& view) {
    const auto label = [](const QString& name) {
        const std::string raw = name.toStdString();
        const std::string_view text = color::displayViewLabel(raw);
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    };
    const QString displayLabel = label(display);
    const QString viewLabel = label(view);
    return displayLabel == viewLabel ? viewLabel : QStringLiteral("%1 / %2").arg(displayLabel, viewLabel);
}

inline constexpr std::array<int, 5> kZoomPresets = {25, 50, 100, 200, 400};
inline constexpr int kFixedZoomItemCount = 1 + static_cast<int>(kZoomPresets.size());
inline constexpr std::array<const char*, 4> kResolutionNames = {"Auto", "Full", "Half", "Quarter"};
// The Resolution dropdown's compact form, item for item.
inline constexpr std::array<const char*, 4> kResolutionCompactNames = {"Auto", "Full", "½", "¼"};
// Task VIEW-1. Channel order is the artist's reading order, not the buffer's byte order: the
// composite first, then the composite without alpha, then the three colour channels, then alpha.
inline constexpr std::array<const char*, 6> kChannelNames = {"RGBA", "RGB", "R", "G", "B", "Alpha"};
// The surround names, in ViewerBackground order; also the stored spellings.
inline constexpr std::array<const char*, 4> kBackgroundNames = {"Solid", "Grey", "Black", "White"};
inline constexpr auto kResolutionSetting = "viewer/resolution";
inline constexpr auto kBackgroundSetting = kViewerBackgroundSettingKey;
inline constexpr auto kTransparencyGridSetting = kViewerTransparencyGridSettingKey;
// Deliberately the SAME key the timeline's own View > Frames/Timecode pair already writes: the
// display format of time is one question with one answer, and a second key would let the ruler and
// the footer disagree about it.
inline constexpr auto kTimeFormatSetting = "timeline/time-format";
inline constexpr auto kLoopSetting = "playback/loop";
inline constexpr auto kSafeAreasSetting = "viewer/overlay/safe-areas";
inline constexpr auto kCentreCrossSetting = "viewer/overlay/centre-cross";
inline constexpr auto kThirdsSetting = "viewer/overlay/thirds";
inline constexpr auto kRulersSetting = "viewer/overlay/rulers";
inline constexpr auto kPixelGridSetting = "viewer/overlay/pixel-grid";

// The footer's collapse order (docs/ux/ui-grammar.md, "Viewer footer"): the lowest rank leaves the
// row first. Transport play/step, loop and the timecode readout never collapse.
enum CollapseRank : int {
    CollapseLook = 1,
    CollapseDisplayView = 2,
    CollapseViewAdjust = 3,
    CollapseRoi = 4,
    CollapseTransparencyGrid = 5,
    CollapseChannel = 6,
    CompactResolution = 7,
    CollapseRamPreview = 8,
    CollapseStepToEnds = 9,
    CollapseFraming = 10,
};

[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveViewerColorConfig(const CompositionSession& session);
[[nodiscard]] bool hasLookTaggedEffect(const CompositionSession& session);
[[nodiscard]] QString safeAreaPresetName(ViewerSafeAreaPreset preset);
[[nodiscard]] ViewerSafeAreaPreset safeAreaPresetFromName(const QString& name);
[[nodiscard]] QString safeAreaPresetSetting(document::CompositionId id);
// What the Resolution control is ACTUALLY delivering: the chosen policy, and for Auto the factor
// it resolved to, which is visible nowhere else.
[[nodiscard]] QString viewerResolutionText(const CompositionPreviewController& controller);
// The footer's icon button: a kit::KIconButton (a QToolButton, so every existing
// findChild<QToolButton*> contract holds) carrying a Control-role glyph in one Control-sized hit
// target. An icon never replaces an accessible name (ADR 0010), so every call site sets a tooltip
// AND an accessible name.
[[nodiscard]] QToolButton* makeFooterButton(kit::IconId iconId, const QString& toolTip,
                                            const QString& accessibleName,
                                            const QString& objectName, QWidget* parent);
// The Viewer's exposure or gamma field: compact, labelled inside, at ViewerAdjustWidth.
[[nodiscard]] kit::KValueField* makeAdjustField(const QString& label, const QString& accessibleName,
                                                const QString& objectName, double minimum,
                                                double maximum, double step, QWidget* parent);
// Gives `field` a dot while it differs from `defaultValue`, and a reset on a double-click on its
// label or from its context menu (which also offers `resetBoth`).
void attachDefaultReset(kit::KValueField& field, double defaultValue, QString resetLabel,
                        QAction* resetBoth);
// The Viewer surround a stored preference names.
[[nodiscard]] ViewerBackground viewerBackgroundFor(ViewerBackgroundPreference preference) noexcept;
// The surround colour for a background choice: Solid is the Canvas token, Grey/Black/White the
// exact CanvasGrey/CanvasBlack/CanvasWhite tokens.
[[nodiscard]] QColor viewerSurroundColor(ViewerBackground background);
// A composition's background colour as the Viewer presents it inside the frame. The authored value
// is display-referred straight RGBA (docs/architecture/color-management.md), so it is quantized
// exactly like its Composition Settings chip and shown opaque; null shows the Canvas token.
[[nodiscard]] QColor compositionFrameColor(const document::Composition* composition);
// Paints the surround over `bounds` (the whole content area).
void drawCanvasBackground(QPainter& painter, const QRectF& bounds, ViewerBackground background);
// What the Viewer paints inside the composition frame under the image: the composition's
// background colour, or the Viewer checker when the Transparency Grid is on (on the same grid as
// the content area, so it matches the GPU present).
void drawFrameBackground(QPainter& painter, const QRectF& content, const QRectF& frame,
                         const QColor& frameColor, bool transparencyGrid);
} // namespace viewer_detail
} // namespace bloom::ui
