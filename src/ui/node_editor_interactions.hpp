#pragma once
#include "node_editor_items.hpp"
#include <QGraphicsRectItem>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <functional>

namespace bloom::ui {
// This is transient canvas state. Frozen revision and stable IDs prevent a gesture from editing
// replacement project content. No draft, mutable document or command stack is retained by UI.
struct NodeInteraction final {
    enum class Mode { Idle, Move, Resize, Box, Link, Cut };
    Mode mode = Mode::Idle;
    document::Revision revision;
    QPointF origin;
    std::map<document::NodeId, QPointF> positions;
    std::set<document::NodeId> baseSelection;
    document::NodeId resized;
    qreal width = 0;
    bool floating = false;
    std::optional<document::InputPortRef> input;
    std::optional<document::OutputPortRef> output;
    std::optional<document::InputPortRef> pickedInput;
    QGraphicsPathItem* line = nullptr;
    QGraphicsRectItem* box = nullptr;
    node_editor::NodeEdgeItem* insertEdge = nullptr;
};
} // namespace bloom::ui
