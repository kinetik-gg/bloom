#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

class QWidget;

namespace bloom::ui::kit {

// One file dialog request (lane C). Every Bloom file dialog goes through kit::chooseFile():
// - it is always parented to the owner's window;
// - it starts in the directory last used for the same `kind` (session state under
//   `dialogs/directories/<kind>` in the user's settings, never project data), or in `startPath`
//   when that names a file or directory;
// - a Save dialog applies `defaultSuffix` when the chosen name has none, also where the platform's
//   native dialog does not;
// - the mounted network shares are added to the sidebar of Qt's own dialog (configureFile-
//   DialogSidebar()). Native platform dialogs keep their own sidebar and ignore that list.
struct KFileRequest final {
    enum class Mode : std::uint8_t { Open, OpenMany, Save, Directory };
    Mode mode = Mode::Open;
    QString title;
    // Remembers the start directory: "project", "media", "export-frame", "openh264", ...
    QString kind;
    // Qt name filters, ";;"-separated ("Bloom Projects (*.bloom)").
    QString nameFilters;
    // Without the dot ("bloom").
    QString defaultSuffix;
    QString startPath;
};

// The chosen paths (empty when cancelled). Synchronous: a file dialog is modal on every platform.
[[nodiscard]] QStringList chooseFiles(QWidget& owner, const KFileRequest& request);
[[nodiscard]] std::optional<QString> chooseFile(QWidget& owner, const KFileRequest& request);

// The directory the next dialog of `kind` starts in, and recording it.
[[nodiscard]] QString rememberedDirectory(const QString& kind);
void rememberDirectory(const QString& kind, const QString& path);

// `path` with `.suffix` appended when its file name has no suffix of its own.
[[nodiscard]] QString withDefaultSuffix(const QString& path, const QString& suffix);

} // namespace bloom::ui::kit
