#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <optional>

class QFileDialog;
class QWidget;

namespace bloom::ui::kit {

// One file dialog request (lane C). Every Bloom file dialog goes through kit::chooseFile():
// - it is always parented to the owner's window;
// - it starts in the directory last used for the same `kind` (session state under
//   `dialogs/directories/<kind>` in the user's settings, never project data), or in `startPath`
//   when that names a file or directory;
// - a Save dialog applies `defaultSuffix` when the chosen name has none;
// - it is Bloom's own (Qt's widget) dialog on every platform, never a native or desktop chooser,
//   styled by the kit, and the mounted network shares are in its sidebar
//   (configureFileDialogSidebar()).
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

// The dialog kit::chooseFiles() runs: always Qt's widget file dialog (never a platform or
// desktop-portal one, on Linux, macOS and Windows alike), window-modal over the owner's window and
// styled entirely by the kit. Exposed for review captures and tests.
[[nodiscard]] std::unique_ptr<QFileDialog> buildFileDialog(QWidget& owner,
                                                           const KFileRequest& request);

// The chosen paths (empty when cancelled). Synchronous: a file dialog is modal on every platform.
[[nodiscard]] QStringList chooseFiles(QWidget& owner, const KFileRequest& request);
[[nodiscard]] std::optional<QString> chooseFile(QWidget& owner, const KFileRequest& request);

// The directory the next dialog of `kind` starts in, and recording it.
[[nodiscard]] QString rememberedDirectory(const QString& kind);
void rememberDirectory(const QString& kind, const QString& path);

// `path` with `.suffix` appended when its file name has no suffix of its own.
[[nodiscard]] QString withDefaultSuffix(const QString& path, const QString& suffix);

} // namespace bloom::ui::kit
