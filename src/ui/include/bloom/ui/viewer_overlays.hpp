#pragma once

#include <bloom/document/composition_settings.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <QLineF>
#include <QRectF>
#include <QSize>
#include <QTransform>
#include <bloom/render/text_raster.hpp>

#include <span>

class QPainter;

namespace bloom::ui {

struct ViewerMapping final {
    // The frozen composition display rectangle (already accounts for proxy scaling and pixel
    // aspect). Before task U3 (issue #119) this was always fitDisplayRect()'s fit-to-window
    // rectangle; ViewerEditor now derives it from the Viewer's own active zoom/pan ViewTransform at
    // gesture begin (viewTransformedDisplayRect() in viewer_editor.cpp) -- fitDisplayRect() exactly
    // when the transform is in Fit mode, or the actively zoomed/panned rectangle otherwise. The
    // freeze contract here is unchanged: this struct still doesn't know or care which geometry
    // source produced the rectangle, only that it was non-empty and is now frozen.
    QRectF displayRect;
    // The frozen composition format; its width/height are the "compositionWidth"/
    // "compositionHeight" of the displacement formulas.
    document::CompositionFormat compositionFormat;
    // The frozen proxy factor, if any (today always CompositionFormatResolution{} -- no proxy
    // pipeline exists yet).
    runtime::EvaluationResolution resolution;
    // The frozen pixel aspect of the displayed frame.
    core::PixelAspectRatio pixelAspect;
    // The frozen display descriptor identity (extent, pixel aspect, and packed layout) used to
    // detect format/proxy/pixel-aspect/descriptor changes.
    render::ReferenceDisplayBufferDescriptor displayDescriptor;

    [[nodiscard]] QPointF toScreen(document::Vec2d point) const;
    [[nodiscard]] document::Vec2d toComposition(QPointF point) const;
    friend bool operator==(const ViewerMapping&, const ViewerMapping&) = default;
};

enum class ViewerHitRegion : unsigned char { Empty, Move, Scale, Rotate, Anchor };
struct ViewerHit final {
    document::LayerId layerId;
    ViewerHitRegion region = ViewerHitRegion::Empty;
    // Clockwise, alternating corners and edge midpoints, starting at top left.
    int handle = 0;
};
[[nodiscard]] std::array<QPointF, 8>
viewerHandlePoints(const ViewerMapping& mapping, const runtime::EvaluatedOperationBounds& bounds);
[[nodiscard]] ViewerHit
hitTestViewer(const ViewerMapping& mapping, QPointF screenPoint,
              std::span<const runtime::EvaluatedOperationBounds> topmostFirst,
              std::span<const runtime::EvaluatedOperationBounds> selected,
              std::span<const document::LayerId> pointText = {});

[[nodiscard]] QLineF textLayoutCaret(const render::TextLayout& layout, std::size_t byte);
[[nodiscard]] std::size_t nearestTextLayoutBoundary(const render::TextLayout& layout,
                                                    QPointF point);
void paintViewerTextEdit(QPainter& painter, const QRectF& clip, const QTransform& transform,
                         const render::TextLayout& layout, std::size_t cursor, std::size_t first,
                         std::size_t last, bool preedit, bool caretVisible);

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
void paintViewerOverlays(QPainter& painter, const QRectF& canvasRect, const ViewerMapping& mapping,
                         double effectiveZoom, const ViewerOverlayOptions& options,
                         std::span<const runtime::EvaluatedOperationBounds> bounds,
                         std::span<const document::LayerId> pointText = {});

} // namespace bloom::ui
