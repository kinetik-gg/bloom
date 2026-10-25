#include "reveal_in_file_manager.hpp"

#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#if defined(BLOOM_UI_HAS_QTDBUS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif

namespace bloom::ui {
namespace {
QString text(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.generic_u16string());
}
void openFolder(const QString& folder) {
    if (!folder.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}
} // namespace

RevealPlatform currentRevealPlatform() noexcept {
#if defined(Q_OS_MACOS)
    return RevealPlatform::MacOS;
#elif defined(Q_OS_WIN)
    return RevealPlatform::Windows;
#else
    return RevealPlatform::Linux;
#endif
}

RevealPlan revealPlan(const RevealPlatform platform, const std::filesystem::path& file) {
    RevealPlan plan;
    const auto path = text(file);
    plan.folder = text(file.parent_path());
    switch (platform) {
    case RevealPlatform::Linux:
        plan.kind = RevealPlan::Kind::ShowItems;
        plan.uris = {QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)};
        break;
    case RevealPlatform::MacOS:
        plan.kind = RevealPlan::Kind::Process;
        plan.program = QStringLiteral("open");
        plan.arguments = {QStringLiteral("-R"), path};
        break;
    case RevealPlatform::Windows: {
        // explorer reads its own command line: /select, then the quoted native path.
        auto native = path;
        native.replace(QLatin1Char('/'), QLatin1Char('\\'));
        plan.kind = RevealPlan::Kind::Process;
        plan.program = QStringLiteral("explorer.exe");
        plan.arguments = {QStringLiteral("/select,"), native};
        plan.nativeArguments = QStringLiteral("/select,\"%1\"").arg(native);
        break;
    }
    }
    return plan;
}

void revealInFileManager(const std::filesystem::path& file, QObject* context) {
    if (file.empty())
        return;
    const auto plan = revealPlan(currentRevealPlatform(), file);
    if (plan.kind == RevealPlan::Kind::Process) {
        QProcess process;
        process.setProgram(plan.program);
#if defined(Q_OS_WIN)
        process.setNativeArguments(plan.nativeArguments);
#else
        process.setArguments(plan.arguments);
#endif
        if (!process.startDetached())
            openFolder(plan.folder);
        return;
    }
#if defined(BLOOM_UI_HAS_QTDBUS)
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && context != nullptr) {
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.FileManager1"),
            QStringLiteral("/org/freedesktop/FileManager1"),
            QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
        message << plan.uris << QString();
        auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), context);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                         [watcher, folder = plan.folder] {
                             // No file manager answered: open the folder, as the fallback.
                             if (watcher->isError())
                                 openFolder(folder);
                             watcher->deleteLater();
                         });
        return;
    }
#else
    static_cast<void>(context);
#endif
    openFolder(plan.folder);
}
} // namespace bloom::ui
