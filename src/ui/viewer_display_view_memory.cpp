#include <bloom/ui/viewer_display_view_memory.hpp>

#include <QCryptographicHash>
#include <QSettings>

namespace bloom::ui {

void ViewerDisplayViewMemory::beginProject(
    const std::optional<std::filesystem::path>& projectPath) {
    projectPath_ = projectPath;
    choices_.clear();
}

void ViewerDisplayViewMemory::moveProject(const std::optional<std::filesystem::path>& projectPath) {
    if (projectPath_ == projectPath)
        return;
    projectPath_ = projectPath;
    for (const auto& [revision, displayView] : choices_)
        save(revision, displayView);
}

std::optional<QStringList> ViewerDisplayViewMemory::recall(const QString& configRevision) const {
    if (const auto found = choices_.find(configRevision); found != choices_.end())
        return found->second;
    if (!projectPath_)
        return std::nullopt;
    auto saved = QSettings().value(settingsKey(configRevision)).toStringList();
    if (saved.size() != 2)
        return std::nullopt;
    return saved;
}

void ViewerDisplayViewMemory::remember(const QString& configRevision,
                                       const QStringList& displayView) {
    if (displayView.size() != 2)
        return;
    choices_[configRevision] = displayView;
    save(configRevision, displayView);
}

QString ViewerDisplayViewMemory::settingsKey(const QString& configRevision) const {
    // A digest, so a path's separators and characters never shape the settings key.
    const auto path = QString::fromStdU16String(projectPath_->lexically_normal().u16string());
    const auto digest = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("viewer/project-display-view/%1/%2")
        .arg(QString::fromLatin1(digest), configRevision);
}

void ViewerDisplayViewMemory::save(const QString& configRevision,
                                   const QStringList& displayView) const {
    if (projectPath_)
        QSettings().setValue(settingsKey(configRevision), displayView);
}

} // namespace bloom::ui
