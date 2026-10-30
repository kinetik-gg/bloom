#pragma once
// Where a command puts a card it creates (task N1, items 3, 4 and 9). Qt-free, so the Node editor,
// scripts and MCP place cards by the same rules: the requested point when it is clear, otherwise
// the nearest free point on the layout grid with kNodeLayoutGap of clear space on every side, and
// a new layer's cards in the layer column at their stack position, as Arrange would put them.
#include <bloom/document/graph.hpp>
#include <bloom/document/node_layout.hpp>

#include <optional>
#include <set>

namespace bloom::document {
class Composition;
}

namespace bloom::commands {

// The lattice automatic placement lands on: the Node editor's default dot grid (Size::NodeGrid).
inline constexpr double kNodeLayoutGrid = document::kNodeLayoutGrid;
// The clear space Arrange and automatic placement keep between two cards: four grid steps.
inline constexpr double kNodeLayoutGap = document::kNodeLayoutSpacing;

// A Qt-free upper estimate of the card the Node editor draws for `node`: its title band, one row
// per parameter, per transport-only input and per output, and Merge's stack pills at their slot
// pitch. The editor supplies the measured extent where it has one; this is what a script, an agent
// or a command that creates cards uses for a card it has never seen drawn.
[[nodiscard]] document::NodeCardSize estimatedNodeCardSize(const document::Composition& composition,
                                                           document::NodeId node);

// Every card's extent: the measured one when `measured` carries it, the estimate otherwise.
[[nodiscard]] document::NodeCardSizes nodeCardSizes(const document::Composition& composition,
                                                    const document::NodeCardSizes& measured = {});

// The layout position every node is drawn at: its authored record, or the default grid.
[[nodiscard]] document::Vec2d effectiveNodePosition(const document::Composition& composition,
                                                    document::NodeId node);

// The top-left a card of `size` should take near `preferred`. `preferred` itself is kept whenever
// it is clear of every other card by `acceptGap`; otherwise the nearest grid-aligned position with
// kNodeLayoutGap clear on every side is chosen, so a crowded point moves the new card just past
// its neighbour rather than a whole card height away. `ignored` cards are not obstacles (the ones
// being placed in the same step).
// `acceptSizes` and `acceptSize`, when given, are the extents the "is `preferred` clear" test
// uses for the other cards and for this one instead of `sizes` and `size`: a cursor add judges
// unmeasured cards by their card floor, so an artist's point is not given up over the estimate's
// safety margin.
[[nodiscard]] document::Vec2d placeNodeCard(const document::Composition& composition,
                                            document::NodeCardSize size, document::Vec2d preferred,
                                            const document::NodeCardSizes& sizes,
                                            const std::set<document::NodeId>& ignored,
                                            double acceptGap = kNodeLayoutGap,
                                            const document::NodeCardSizes* acceptSizes = nullptr,
                                            std::optional<document::NodeCardSize> acceptSize = {});

// Every card's floor extent for the acceptance test above: the measured one when given, otherwise
// the authored width raised to the card floor (Size::NodeCardWidth) and the estimated height.
[[nodiscard]] document::NodeCardSizes nodeCardFloorSizes(const document::Composition& composition,
                                                         const document::NodeCardSizes& measured);

// Writes the layout of a just-created layer: its Layer Output in the layer column beside the
// neighbouring layers of its stack (above the layer under it, below the layer over it), and its
// source on the same row to the left -- the order Arrange gives the stack -- each moved off any
// card it would touch. Existing cards are never moved.
void placeLayerCards(document::Composition& composition, document::NodeId layerNode,
                     document::NodeId sourceNode, const document::NodeCardSizes& measured = {});

} // namespace bloom::commands
