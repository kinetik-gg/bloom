#pragma once

// The accept-time check every dialog that edits project items shares (Edit Tags, Input Colour,
// Delete): the project the dialog was opened on is still the one the session holds. A dialog reads
// its values from the snapshot it captured; if the project changed while it was open (an edit, an
// undo, an Open or a recovery), committing those values could overwrite work the dialog never
// showed, so the dialog says so instead and commits nothing.

#include <bloom/document/document.hpp>

#include <memory>

class QObject;

namespace bloom::ui {

class CompositionSession;

class ProjectChangeCheck final {
  public:
    explicit ProjectChangeCheck(CompositionSession& session);
    ~ProjectChangeCheck();
    ProjectChangeCheck(const ProjectChangeCheck&) = delete;
    ProjectChangeCheck& operator=(const ProjectChangeCheck&) = delete;

    // The snapshot the dialog was opened on.
    [[nodiscard]] const document::Snapshot& snapshot() const noexcept { return snapshot_; }
    // Anything in the project changed since then.
    [[nodiscard]] bool projectChanged() const;
    // The session swapped in a different document (Open, New, recovery) since then.
    [[nodiscard]] bool documentRebound() const noexcept { return *rebound_; }

  private:
    CompositionSession& session_;
    document::Snapshot snapshot_;
    std::shared_ptr<bool> rebound_ = std::make_shared<bool>(false);
    std::unique_ptr<QObject> context_;
};

} // namespace bloom::ui
