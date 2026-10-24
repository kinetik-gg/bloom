// ADR 0028 authoring helpers: the Viewer's content <-> composition mapping and primitive paths.
#include "layer_mask_support.hpp"

#include <QCoreApplication>
#include <cmath>
#include <iostream>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
bool near(const double a, const double b) { return std::abs(a - b) < 1e-6; }
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // A 100 x 50 content rectangle drawn rotated by 90 degrees, doubled, at (300, 200).
    runtime::EvaluatedOperationBounds bounds;
    bounds.local = {0, 0, 100, 50};
    bounds.polygon = {document::Vec2d{300, 200}, document::Vec2d{300, 400},
                      document::Vec2d{200, 400}, document::Vec2d{200, 200}};
    const auto world = ui::masks::contentToComposition(bounds);
    expect(world.has_value(), "a non-degenerate polygon yields an affine");
    if (!world)
        return 1;
    const auto corner = world->map(QPointF(100, 0));
    expect(near(corner.x(), 300) && near(corner.y(), 400), "content (100,0) maps to polygon[1]");
    const auto other = world->map(QPointF(0, 50));
    expect(near(other.x(), 200) && near(other.y(), 200), "content (0,50) maps to polygon[3]");
    const auto back = world->inverted().map(world->map(QPointF(37, 11)));
    expect(near(back.x(), 37) && near(back.y(), 11), "the inverse returns content coordinates");

    // Local bounds that do not start at the origin still land on the polygon corners.
    bounds.local = {-10, -20, 90, 30};
    const auto shifted = ui::masks::contentToComposition(bounds);
    expect(shifted && near(shifted->map(QPointF(-10, -20)).x(), 300) &&
               near(shifted->map(QPointF(-10, -20)).y(), 200),
           "the local rectangle origin maps to polygon[0]");

    bounds.local = {0, 0, 0, 0};
    expect(!ui::masks::contentToComposition(bounds), "empty bounds have no mapping");
    bounds.local = {0, 0, 10, 10};
    bounds.polygon = {document::Vec2d{0, 0}, document::Vec2d{0, 0}, document::Vec2d{0, 0},
                      document::Vec2d{0, 0}};
    expect(!ui::masks::contentToComposition(bounds), "a collapsed polygon has no mapping");

    // Primitives and path mapping.
    const auto rect = ui::masks::rectanglePath(QRectF(10, 20, 30, 40));
    expect(rect.closed && rect.anchors.size() == 4 && rect.isValid(), "rectangle: four anchors");
    expect(rect.anchors[2].point.x == 40 && rect.anchors[2].point.y == 60, "rectangle corners");
    const auto ellipse = ui::masks::ellipsePath(QRectF(0, 0, 100, 60));
    expect(ellipse.closed && ellipse.anchors.size() == 4 && ellipse.anchors[1].outHandle &&
               near(ellipse.anchors[1].point.x, 100) &&
               near(ellipse.anchors[1].outHandle->y, 30 + 30 * ui::masks::kEllipseKappa),
           "ellipse: four anchors with kappa handles");
    const QTransform scale = QTransform::fromScale(2, 3);
    const auto mapped = ui::masks::mappedPath(ellipse, scale);
    expect(near(mapped.anchors[1].point.x, 200) &&
               near(mapped.anchors[1].outHandle->y, 3 * ellipse.anchors[1].outHandle->y),
           "mapping transforms points and handles");

    // Outline geometry: an offset square grows and shrinks.
    const auto square = ui::masks::rectanglePath(QRectF(0, 0, 100, 100));
    const auto grown = ui::masks::offsetOutline(square, 5).boundingRect();
    const auto shrunk = ui::masks::offsetOutline(square, -5).boundingRect();
    expect(grown.width() > 100 && shrunk.width() < 100, "positive offsets grow, negative shrink");
    expect(ui::masks::offsetOutline(document::PathValue{}, 5).isEmpty(),
           "empty path: empty bounds");
    auto open = square;
    open.closed = false;
    expect(ui::masks::offsetOutline(open, 5).isEmpty(), "an open path has no feather bounds");

    return failures == 0 ? 0 : 1;
}
