#include <bloom/ui/kit/file_chooser.hpp>

#include <bloom/ui/kit/network_shares.hpp>

#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QWidget>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] QString settingsKey(const QString& kind) {
    return QStringLiteral("dialogs/directories/") + kind;
}

[[nodiscard]] QString startDirectory(const KFileRequest& request) {
    if (!request.startPath.isEmpty())
        return request.startPath;
    return request.kind.isEmpty() ? QString{} : rememberedDirectory(request.kind);
}

} // namespace

QString rememberedDirectory(const QString& kind) {
    const auto stored = QSettings().value(settingsKey(kind)).toString();
    return !stored.isEmpty() && QFileInfo(stored).isDir() ? stored : QString{};
}

void rememberDirectory(const QString& kind, const QString& path) {
    if (kind.isEmpty() || path.isEmpty())
        return;
    const QFileInfo info(path);
    QSettings().setValue(settingsKey(kind),
                         info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

QString withDefaultSuffix(const QString& path, const QString& suffix) {
    if (path.isEmpty() || suffix.isEmpty() || !QFileInfo(path).suffix().isEmpty())
        return path;
    return path + QLatin1Char('.') + suffix;
}

QStringList chooseFiles(QWidget& owner, const KFileRequest& request) {
    QFileDialog dialog(owner.window(), request.title, startDirectory(request), request.nameFilters);
    switch (request.mode) {
    case KFileRequest::Mode::Open:
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setFileMode(QFileDialog::ExistingFile);
        break;
    case KFileRequest::Mode::OpenMany:
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setFileMode(QFileDialog::ExistingFiles);
        break;
    case KFileRequest::Mode::Save:
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setFileMode(QFileDialog::AnyFile);
        break;
    case KFileRequest::Mode::Directory:
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setOption(QFileDialog::ShowDirsOnly);
        break;
    }
    if (!request.defaultSuffix.isEmpty())
        dialog.setDefaultSuffix(request.defaultSuffix);
    configureFileDialogSidebar(dialog);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    auto files = dialog.selectedFiles();
    if (request.mode == KFileRequest::Mode::Save)
        for (auto& file : files)
            file = withDefaultSuffix(file, request.defaultSuffix);
    if (!files.isEmpty())
        rememberDirectory(request.kind, files.front());
    return files;
}

std::optional<QString> chooseFile(QWidget& owner, const KFileRequest& request) {
    const auto files = chooseFiles(owner, request);
    if (files.isEmpty())
        return std::nullopt;
    return files.front();
}

} // namespace bloom::ui::kit
