#pragma once

// ADR 0028 authoring helpers shared by the Viewer, Properties and Timeline: content <-> composition
// mapping for mask paths, primitive mask paths, sampling of a mask's path and its outline geometry.
// Pure functions over document values; nothing here reads or writes a session.

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <optional>
#include <vector>

namespace bloom::ui::masks {

// Kappa for a cubic approximation of a quarter circle.
inline constexpr double kEllipseKappa = 0.5523;

// The affine taking the layer's content pixel space to composition space, from the evaluated
// content rectangle and its composition-space polygon. nullopt when degenerate.
[[nodiscard]] std::optional<QTransform>
contentToComposition(const runtime::EvaluatedOperationBounds& bounds);

[[nodiscard]] document::PathValue mappedPath(const document::PathValue& path,
                                             const QTransform& transform);

// A closed four-anchor rectangle and a closed four-anchor ellipse with cubic handles.
[[nodiscard]] document::PathValue rectanglePath(const QRectF& rect);
[[nodiscard]] document::PathValue ellipsePath(const QRectF& rect);

// The mask path parameter's value at `keyTime` (the parameter's own clock): the constant, or the
// Path curve sampled exactly as the evaluator samples it.
[[nodiscard]] std::optional<document::PathValue>
maskPathAt(const document::Composition& composition, const document::LayerMask& mask,
           core::RationalTime keyTime);

[[nodiscard]] QPainterPath outline(const document::PathValue& path);

// The outline displaced outward by `distance` pixels (negative inward); used for the feather
// bounds at expansion +/- feather / 2. An open or degenerate path yields an empty path.
[[nodiscard]] QPainterPath offsetOutline(const document::PathValue& path, double distance);

[[nodiscard]] QString modeText(document::LayerMaskMode mode);
[[nodiscard]] QString channelText(document::TrackMatteChannel channel);

// Layers of the same Merge as `layer` other than `layer` itself: the track matte source choices.
[[nodiscard]] std::vector<document::LayerId>
matteSourceCandidates(const document::Composition& composition, document::LayerId layer);

} // namespace bloom::ui::masks
