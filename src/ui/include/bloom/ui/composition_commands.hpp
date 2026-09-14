#pragma once

#include <bloom/document/ids.hpp>

#include <optional>

class QWidget;

namespace bloom::ui {

class CompositionSession;

// Shared composition authoring entry points. Every function executes at most one command
// transaction; callers only own the surrounding affordance and selection behavior.
[[nodiscard]] std::optional<document::CompositionId>
showNewCompositionDialog(CompositionSession& session, QWidget* parent = nullptr);

[[nodiscard]] std::optional<document::CompositionId>
duplicateComposition(CompositionSession& session, document::CompositionId source);

[[nodiscard]] bool deleteComposition(CompositionSession& session, document::CompositionId id);

[[nodiscard]] bool renameComposition(CompositionSession& session, document::CompositionId id,
                                     QWidget* parent = nullptr);

} // namespace bloom::ui
