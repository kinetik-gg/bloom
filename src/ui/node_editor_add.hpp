#pragma once
// The Node editor's compound adds are commands (bloom/commands/node_editor_operations.hpp) so a
// script or an MCP agent issues the same undoable step; the editor keeps its own names for them.
#include <bloom/commands/node_editor_operations.hpp>

namespace bloom::ui::node_editor {
using commands::AddCompositionSource;
using commands::AddEditorNode;
using commands::checkAddEditorNodeEligibility;
using commands::InsertReroute;
} // namespace bloom::ui::node_editor
