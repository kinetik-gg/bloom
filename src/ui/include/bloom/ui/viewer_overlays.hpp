#pragma once

#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <QRectF>
#include <QSize>

#include <span>

class QPainter;

namespace bloom::ui {

enum class ViewerSafeAreaPreset : unsigned char {
    Broadcast,
    Hd,
    Cinema,
    Social,
    Custom,
};

struct ViewerOverlayOptions final {
    bool safeAreas = false;
    bool centreCross = false;
    bool thirds = false;
    bool rulers = false;
    bool pixelGrid = false;
    ViewerSafeAreaPreset safeAreaPreset = ViewerSafeAreaPreset::Broadcast;
    document::SafeAreaSettings safeAreaSettings{};
};

// Paints display-only viewer guides in screen space. The helper receives the active transformed
// display rectangle, so every guide follows zoom and pan. Nothing here touches a render buffer or
// document export; `bounds` is the immutable, already-evaluated selection diagnostic only.
void paintViewerOverlays(QPainter& painter, const QRectF& canvasRect, const QRectF& displayRect,
                         QSize compositionSize, double effectiveZoom,
                         const ViewerOverlayOptions& options,
                         std::span<const runtime::EvaluatedOperationBounds> bounds);

} // namespace bloom::ui
