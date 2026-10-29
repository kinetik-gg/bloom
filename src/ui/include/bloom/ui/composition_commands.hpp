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
[[nodiscard]] std::optional<document::CompositionId>
showNewCompositionDialog(CompositionSession& session, QWidget* parent = nullptr,
                         std::optional<document::AssetFolderId> folder = std::nullopt);

// The shared New Composition / Composition Settings modal (#376) in settings mode: edits every
// setting of `id` and commits it as one undoable "Composition Settings" step (see
// applyCompositionSettings()). True when a change was committed.
bool showCompositionSettingsDialog(CompositionSession& session, document::CompositionId id,
                                   QWidget* parent = nullptr);

// Commits `settings` to composition `id` as one undoable "Composition Settings" step. An edit that
// changes only the background colour commits SetCompositionBackgroundColor, which is not
// render-affecting (the background is viewer presentation), so the Viewer re-presents its frame
// without a re-evaluation; any other change commits SetCompositionSettings. True when committed.
bool applyCompositionSettings(CompositionSession& session, document::CompositionId id,
                              const commands::CompositionSettings& settings,
                              commands::ResizeAnchor anchor = {});

[[nodiscard]] std::optional<document::CompositionId>
duplicateComposition(CompositionSession& session, document::CompositionId source);

[[nodiscard]] bool deleteComposition(CompositionSession& session, document::CompositionId id);

[[nodiscard]] bool renameComposition(CompositionSession& session, document::CompositionId id,
                                     QWidget* parent = nullptr);

} // namespace bloom::ui
