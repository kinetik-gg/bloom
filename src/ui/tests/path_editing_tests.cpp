// The shared post-confirmation path editing core (masks and Pen shape paths alike): the five handle
// modes and their drag behaviour, mode switching with a mixed selection, Auto recomputation, the
// shape-preserving split, deletion with its minimum, closing and reversing.
#include <bloom/ui/path_editing.hpp>

#include <cmath>
#include <iostream>
#include <string>

using namespace bloom;
using document::PathAnchor;
using document::PathAnchorMode;
using document::PathValue;
using document::Vec2d;
namespace pe = bloom::ui::path_editing;

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
bool near(const double a, const double b, const double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}
bool near(const Vec2d a, const Vec2d b, const double tolerance = 1e-9) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
}
double length(const Vec2d a, const Vec2d b) { return std::hypot(a.x - b.x, a.y - b.y); }
// Positive when b - p and p - a point the same way (collinear through p, opposed handles).
bool collinear(const PathAnchor& anchor) {
    const auto& p = anchor.point;
    const auto& in = *anchor.inHandle;
    const auto& out = *anchor.outHandle;
    const double cross = (in.x - p.x) * (out.y - p.y) - (in.y - p.y) * (out.x - p.x);
    const double dot = (in.x - p.x) * (out.x - p.x) + (in.y - p.y) * (out.y - p.y);
    return std::abs(cross) < 1e-6 && dot < 0.0;
}

// An open four-anchor S: a corner, a mirrored, an aligned and a free anchor.
PathValue sample() {
    PathValue path;
    path.anchors = {{{0, 0}, std::nullopt, std::nullopt, PathAnchorMode::Corner},
                    {{100, 0}, Vec2d{80, -20}, Vec2d{120, 20}, PathAnchorMode::Mirrored},
                    {{200, 0}, Vec2d{190, -10}, Vec2d{230, 30}, PathAnchorMode::Aligned},
                    {{300, 0}, Vec2d{280, 30}, Vec2d{320, 40}, PathAnchorMode::Free}};
    return path;
}

std::vector<Vec2d> outlineSamples(const PathValue& path) {
    std::vector<Vec2d> result;
    for (std::size_t segment = 0; segment < pe::segmentCount(path); ++segment) {
        const auto controls = pe::segmentControls(path, segment);
        for (int step = 0; step <= 64; ++step)
            result.push_back(pe::segmentPoint(controls, step / 64.0));
    }
    return result;
}
// Every point of `a` lies on `b` (dense nearest sample), so the drawn shape is unchanged.
bool sameShape(const PathValue& a, const PathValue& b) {
    const auto dense = [](const PathValue& path) {
        std::vector<Vec2d> result;
        for (std::size_t segment = 0; segment < pe::segmentCount(path); ++segment) {
            const auto controls = pe::segmentControls(path, segment);
            for (int step = 0; step <= 4096; ++step)
                result.push_back(pe::segmentPoint(controls, step / 4096.0));
        }
        return result;
    };
    const auto target = dense(b);
    for (const auto& point : outlineSamples(a)) {
        double best = 1e300;
        for (const auto& other : target)
            best = std::min(best, length(point, other));
        if (best > 0.05)
            return false;
    }
    return true;
}
} // namespace

int main() {
    // Modes, and the inference that migrates paths written before the mode existed.
    {
        const auto path = sample();
        expect(document::inferPathAnchorMode(path.anchors[0]) == PathAnchorMode::Corner,
               "no handles infers Corner");
        expect(document::inferPathAnchorMode(path.anchors[1]) == PathAnchorMode::Mirrored,
               "collinear equal handles infer Mirrored");
        expect(document::inferPathAnchorMode(path.anchors[2]) == PathAnchorMode::Aligned,
               "collinear unequal handles infer Aligned");
        expect(document::inferPathAnchorMode(path.anchors[3]) == PathAnchorMode::Free,
               "independent handles infer Free");
        PathAnchor oneHandle{{0, 0}, std::nullopt, Vec2d{5, 5}};
        expect(document::inferPathAnchorMode(oneHandle) == PathAnchorMode::Free,
               "a single handle infers Free");
        PathAnchor legacy{{100, 0}, Vec2d{80, -20}, Vec2d{120, 20}};
        expect(pe::modeOf(legacy) == PathAnchorMode::Mirrored,
               "a Corner-labelled anchor with handles reads as its geometry's mode");
        for (const auto mode :
             {PathAnchorMode::Corner, PathAnchorMode::Mirrored, PathAnchorMode::Aligned,
              PathAnchorMode::Free, PathAnchorMode::Auto})
            expect(document::pathAnchorModeFromName(document::pathAnchorModeName(mode)) == mode,
                   "every mode name round-trips");
    }

    // Each mode's handle drag.
    {
        auto path = sample();
        pe::dragHandle(path, 1, pe::HandleSide::Out, {130, 40}, false);
        expect(near(*path.anchors[1].inHandle, {70, -40}),
               "Mirrored: the opposite handle mirrors direction and length");
        expect(path.anchors[1].mode == PathAnchorMode::Mirrored, "Mirrored stays Mirrored");

        const auto inLength = length(*path.anchors[2].inHandle, path.anchors[2].point);
        pe::dragHandle(path, 2, pe::HandleSide::Out, {200, 50}, false);
        expect(collinear(path.anchors[2]) &&
                   near(length(*path.anchors[2].inHandle, path.anchors[2].point), inLength),
               "Aligned: the opposite handle rotates onto the tangent and keeps its length");
        expect(near(*path.anchors[2].inHandle, {200, -inLength}),
               "Aligned: the opposite handle points straight away");

        const auto freeIn = *path.anchors[3].inHandle;
        pe::dragHandle(path, 3, pe::HandleSide::Out, {350, -60}, false);
        expect(near(*path.anchors[3].inHandle, freeIn), "Free: the opposite handle stays put");

        auto mirrored = sample();
        const auto before = *mirrored.anchors[1].inHandle;
        pe::dragHandle(mirrored, 1, pe::HandleSide::Out, {150, 10}, true);
        expect(near(*mirrored.anchors[1].inHandle, before) &&
                   mirrored.anchors[1].mode == PathAnchorMode::Free,
               "an Alt drag leaves the opposite handle alone and makes the anchor Free");

        auto automatic = sample();
        pe::setMode(automatic, {1}, PathAnchorMode::Auto);
        pe::dragHandle(automatic, 1, pe::HandleSide::Out, {140, 20}, false);
        expect(automatic.anchors[1].mode == PathAnchorMode::Aligned &&
                   collinear(automatic.anchors[1]),
               "dragging an Auto handle makes the anchor Aligned");
    }

    // Switching modes, including a mixed multi-selection.
    {
        auto path = sample();
        expect(!pe::commonMode(path, {0, 1, 2, 3}).has_value(), "a mixed selection has no mode");
        expect(pe::commonMode(path, {1}) == PathAnchorMode::Mirrored, "one anchor's mode");
        pe::setMode(path, {0, 1, 2, 3}, PathAnchorMode::Aligned);
        expect(pe::commonMode(path, {0, 1, 2, 3}) == PathAnchorMode::Aligned,
               "the selection converts to one mode");
        for (std::size_t index = 0; index < 4; ++index)
            expect(path.anchors[index].inHandle && path.anchors[index].outHandle &&
                       collinear(path.anchors[index]),
                   "every converted anchor has collinear handles");
        pe::setMode(path, {2}, PathAnchorMode::Mirrored);
        expect(near(length(*path.anchors[2].inHandle, path.anchors[2].point),
                    length(*path.anchors[2].outHandle, path.anchors[2].point)),
               "Mirrored equalises the handle lengths");
        pe::setMode(path, {1, 3}, PathAnchorMode::Corner);
        expect(!path.anchors[1].inHandle && !path.anchors[3].outHandle &&
                   pe::modeOf(path.anchors[1]) == PathAnchorMode::Corner,
               "Corner drops the handles");
        pe::setMode(path, {1}, PathAnchorMode::Free);
        expect(path.anchors[1].inHandle && path.anchors[1].outHandle,
               "a curved mode pulls handles out of a Corner");

        auto toggled = sample();
        pe::toggleCorner(toggled, 1, PathAnchorMode::Aligned);
        expect(pe::modeOf(toggled.anchors[1]) == PathAnchorMode::Corner,
               "Alt-click: curved -> Corner");
        pe::toggleCorner(toggled, 1, PathAnchorMode::Aligned);
        expect(toggled.anchors[1].mode == PathAnchorMode::Aligned && toggled.anchors[1].outHandle,
               "Alt-click: Corner -> the previous curved mode");
        pe::pullHandles(toggled, 0, {10, 10});
        expect(toggled.anchors[0].mode == PathAnchorMode::Mirrored &&
                   near(*toggled.anchors[0].inHandle, {-10, -10}),
               "an Alt drag from a point pulls symmetric handles");
    }

    // Auto recomputes from its neighbours whenever they move.
    {
        auto path = sample();
        pe::setMode(path, {1}, PathAnchorMode::Auto);
        expect(near(*path.anchors[1].outHandle, {100 + 200.0 / 6.0, 0}) &&
                   near(*path.anchors[1].inHandle, {100 - 200.0 / 6.0, 0}),
               "Auto handles are (next - previous) / 6 either side");
        pe::moveAnchors(path, {2}, {0, 60});
        expect(near(*path.anchors[1].outHandle, {100 + 200.0 / 6.0, 10}),
               "moving a neighbour recomputes the Auto anchor");
        pe::moveAnchors(path, {0}, {0, -60});
        expect(near(*path.anchors[1].inHandle, {100 - 200.0 / 6.0, -20}),
               "moving the other neighbour recomputes it too");
    }

    // Adding a point on a segment keeps the shape.
    {
        auto path = sample();
        const auto original = path;
        auto added = pe::insertAnchor(path, {1, 0.37, 0});
        expect(added == 2 && path.anchors.size() == 5, "the split inserts after the segment start");
        expect(sameShape(original, path) && sameShape(path, original),
               "a curved split leaves the drawn shape unchanged");
        auto straight = PathValue{};
        straight.closed = true;
        straight.anchors = {{{0, 0}, std::nullopt, std::nullopt},
                            {{100, 0}, std::nullopt, std::nullopt},
                            {{100, 100}, std::nullopt, std::nullopt}};
        const auto square = straight;
        added = pe::insertAnchor(straight, {2, 0.5, 0});
        expect(added == 3 && near(straight.anchors[3].point, {50, 50}) &&
                   straight.anchors[3].mode == PathAnchorMode::Corner &&
                   sameShape(square, straight),
               "a straight closing-segment split adds a Corner on the line");
        const auto toScreen = [](const Vec2d point) { return Vec2d{point.x * 2, point.y * 2}; };
        const auto hit = pe::nearestSegmentPoint(square, toScreen, {200, 2});
        expect(hit && hit->segment == 1 && near(hit->distance, 0, 1e-6) &&
                   near(pe::segmentPoint(pe::segmentControls(square, 1), hit->t), {100, 1}, 1e-6),
               "the nearest segment point is found in screen space");
    }

    // Deleting points keeps a valid path; the minimum refuses.
    {
        auto path = sample();
        expect(pe::deleteAnchors(path, {1, 2}, 2) && path.anchors.size() == 2,
               "a shape path keeps two anchors");
        expect(!pe::deleteAnchors(path, {0}, 2) && path.anchors.size() == 2,
               "below the shape minimum is refused");
        PathValue mask;
        mask.closed = true;
        mask.anchors = {{{0, 0}, std::nullopt, std::nullopt},
                        {{10, 0}, std::nullopt, std::nullopt},
                        {{10, 10}, std::nullopt, std::nullopt},
                        {{0, 10}, std::nullopt, std::nullopt}};
        expect(pe::deleteAnchors(mask, {3}, pe::minimumAnchors(true)) && mask.closed &&
                   mask.anchors.size() == 3,
               "a closed mask keeps three anchors");
        expect(!pe::deleteAnchors(mask, {0}, pe::minimumAnchors(true)) && mask.anchors.size() == 3,
               "a mask below three anchors is refused");
        PathValue closedShape = mask;
        expect(pe::deleteAnchors(closedShape, {0}, pe::minimumAnchors(false)) &&
                   !closedShape.closed,
               "a closed shape path left with two anchors opens");
    }

    // Open / close and reversing for continuing from the first anchor.
    {
        auto path = sample();
        expect(pe::setClosed(path, true) && path.closed, "an open path closes");
        expect(pe::setClosed(path, false) && !path.closed, "and opens again");
        PathValue pair;
        pair.anchors = {{{0, 0}, std::nullopt, std::nullopt}, {{1, 1}, std::nullopt, std::nullopt}};
        expect(!pe::setClosed(pair, true), "two anchors cannot close");
        auto reversed = sample();
        pe::reverse(reversed);
        expect(near(reversed.anchors[0].point, {300, 0}) &&
                   near(*reversed.anchors[0].outHandle, {280, 30}) &&
                   near(*reversed.anchors[0].inHandle, {320, 40}),
               "reversing swaps order and handles");
    }

    if (failures != 0)
        return 1;
    std::cout << "path editing PASS\n";
    return 0;
}
