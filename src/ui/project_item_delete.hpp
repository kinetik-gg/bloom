#pragma once

// The one delete path for project items (#378, lane C). Assets' Delete, Composition > Delete
// Composition and the Viewer's Delete Composition all come here, so a used item is never deleted
// without the same warning: an unused item or an empty folder goes at once; a used item, or a
// folder that holds items, asks first (docs/ux/interaction-model.md, Assets). Every delete is one
// transaction and one undo step, and deleting the active composition switches to the remaining
// one with the lowest id.

#include <bloom/document/ids.hpp>

#include <vector>

class QWidget;

namespace bloom::ui {

class CompositionSession;

struct ProjectItemSelection final {
    std::vector<document::AssetId> assets;
    std::vector<document::CompositionId> compositions;
    std::vector<document::AssetFolderId> folders;
};

// Deletes `selection`, asking first when the warning applies. The question is asynchronous and
// window-modal over `owner`; a project that changed while it was open deletes nothing.
void requestDeleteProjectItems(CompositionSession& session, QWidget& owner,
                               ProjectItemSelection selection);

} // namespace bloom::ui
