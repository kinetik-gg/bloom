#pragma once

#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/document/ids.hpp>

#include <optional>

class QWidget;

namespace bloom::ui {

class CompositionSession;

// Shared composition authoring entry points. Every function executes at most one command
// transaction; callers only own the surrounding affordance and selection behavior.
// `folder` is the Assets folder the new composition is filed in (#378); nullopt is the root.
// The Composition dialog is modal over `owner`'s window and returns the new composition, so the
// caller can open or select it.
[[nodiscard]] std::optional<document::CompositionId>
showNewCompositionDialog(CompositionSession& session, QWidget& owner,
                         std::optional<document::AssetFolderId> folder = std::nullopt);

// The shared New Composition / Composition Settings modal (#376) in settings mode: edits every
// setting of `id` and commits it as one undoable "Composition Settings" step (see
// applyCompositionSettings()). True when a change was committed.
bool showCompositionSettingsDialog(CompositionSession& session, document::CompositionId id,
                                   QWidget& owner);

// Commits `settings` to composition `id` as one undoable "Composition Settings" step. An edit that
// changes only the background colour commits SetCompositionBackgroundColor, which is not
// render-affecting (the background is viewer presentation), so the Viewer re-presents its frame
// without a re-evaluation; any other change commits SetCompositionSettings. True when committed.
bool applyCompositionSettings(CompositionSession& session, document::CompositionId id,
                              const commands::CompositionSettings& settings,
                              commands::ResizeAnchor anchor = {});

[[nodiscard]] std::optional<document::CompositionId>
duplicateComposition(CompositionSession& session, document::CompositionId source);

// The dialog-free delete primitive: one "Delete Composition" step, used or not.
[[nodiscard]] bool deleteComposition(CompositionSession& session, document::CompositionId id);

// Composition > Delete Composition and the Viewer's Delete Composition: the same delete path, and
// the same in-use warning, as deleting the composition in Assets (project_item_delete.hpp).
void requestDeleteComposition(CompositionSession& session, document::CompositionId id,
                              QWidget& owner);

// Asks for the new name (asynchronous, window-modal over `owner`) and commits one "Rename
// Composition" step. A name that cannot be used keeps the prompt open with its reason.
void renameComposition(CompositionSession& session, document::CompositionId id, QWidget& owner);

} // namespace bloom::ui
