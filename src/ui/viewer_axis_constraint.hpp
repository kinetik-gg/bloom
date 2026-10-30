#pragma once

// Shift-constrained move shared by the layer move (CompositionSession) and the path-point move
// (ViewerEditor): the TOTAL drag vector since the press snaps to the nearest multiple of 45 degrees
// and the vector is projected onto that direction. Being a pure function of the total vector, a
// mid-drag Shift press or release never accumulates error.

#include <QPointF>

#include <cmath>

namespace bloom::ui {

[[nodiscard]] inline QPointF constrainToAxis45(const QPointF delta) noexcept {
    constexpr double step = 3.14159265358979323846 / 4.0;
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()) ||
        (delta.x() == 0.0 && delta.y() == 0.0))
        return delta;
    const double angle = std::round(std::atan2(delta.y(), delta.x()) / step) * step;
    const QPointF direction(std::cos(angle), std::sin(angle));
    const double along = QPointF::dotProduct(delta, direction);
    // Axis components that are mathematically zero are exactly zero, never a rounding residue.
    return {std::abs(direction.x()) < 1e-9 ? 0.0 : direction.x() * along,
            std::abs(direction.y()) < 1e-9 ? 0.0 : direction.y() * along};
}

} // namespace bloom::ui
