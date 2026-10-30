#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>
#include <map>
#include <optional>

namespace bloom::ui {

// The Viewer's remembered Display / View choice. It is session state, never project truth
// (docs/architecture/color-management.md, "Viewer display/view and look selection"): choosing a
// view changes no document, history entry or export. A choice is remembered per project and per
// OCIO config revision, so it survives composition switches, Viewer re-creation and, for a saved
// project, closing and reopening it: a saved project's choices live in QSettings under a digest of
// its file path. An unsaved project keeps its choices in memory only. A project with no remembered
// choice starts on its config's Viewer default, which is never tone-mapped.
class ViewerDisplayViewMemory final {
  public:
    // New or Open installed another project: forget the in-memory choices. `projectPath` is the
    // installed project's file, or nullopt for an unsaved one.
    void beginProject(const std::optional<std::filesystem::path>& projectPath);
    // Save As gave the same project a new file: keep its choices and save them under that file.
    void moveProject(const std::optional<std::filesystem::path>& projectPath);
    [[nodiscard]] const std::optional<std::filesystem::path>& projectPath() const noexcept {
        return projectPath_;
    }

    // The remembered {display, view} for a config revision (lowercase hex), if any.
    [[nodiscard]] std::optional<QStringList> recall(const QString& configRevision) const;
    void remember(const QString& configRevision, const QStringList& displayView);

  private:
    [[nodiscard]] QString settingsKey(const QString& configRevision) const;
    void save(const QString& configRevision, const QStringList& displayView) const;

    std::optional<std::filesystem::path> projectPath_;
    std::map<QString, QStringList> choices_;
};

} // namespace bloom::ui
