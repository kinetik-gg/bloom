#include <bloom/ui/kit/network_shares.hpp>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bloom::ui::kit {
namespace {

constexpr auto kSmbSharePrefix = QLatin1String("smb-share:");

struct ParsedMountName {
    QString server;
    QString share;
};

// Parses a GVFS mount directory's own name, e.g. `smb-share:server=10.10.10.20,share=production`
// or `smb-share:domain=WORKGROUP,server=host,share=archive,user=alice` (fields in any order;
// unrecognized fields, such as `domain=`/`user=`, are ignored).
[[nodiscard]] std::optional<ParsedMountName> parseMountName(const QString& entryName) {
    if (!entryName.startsWith(kSmbSharePrefix))
        return std::nullopt;
    ParsedMountName parsed;
    const auto fields = entryName.mid(kSmbSharePrefix.size()).split(QLatin1Char(','));
    for (const auto& field : fields) {
        const auto equals = field.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;
        const auto key = field.left(equals);
        const auto value = field.mid(equals + 1);
        if (key.compare(QLatin1String("server"), Qt::CaseInsensitive) == 0)
            parsed.server = value;
        else if (key.compare(QLatin1String("share"), Qt::CaseInsensitive) == 0)
            parsed.share = value;
    }
    if (parsed.server.isEmpty() || parsed.share.isEmpty())
        return std::nullopt;
    return parsed;
}

struct ParsedSmbUrl {
    QString host;
    QString share;
    QString rest;
};

// Splits `smb://host/share/rest...` into its three parts. `url.path()` is percent-decoded
// (QUrl's default PrettyDecoded), which is the "rest (percent-decoded)" the mapping contract
// asks for.
[[nodiscard]] std::optional<ParsedSmbUrl> parseSmbUrl(const QUrl& url) {
    if (url.scheme().compare(QLatin1String("smb"), Qt::CaseInsensitive) != 0)
        return std::nullopt;
    const auto host = url.host();
    if (host.isEmpty())
        return std::nullopt;
    auto path = url.path();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    if (path.isEmpty())
        return std::nullopt;
    const auto slash = path.indexOf(QLatin1Char('/'));
    ParsedSmbUrl parsed;
    parsed.host = host;
    parsed.share = slash < 0 ? path : path.left(slash);
    parsed.rest = slash < 0 ? QString() : path.mid(slash + 1);
    if (parsed.share.isEmpty())
        return std::nullopt;
    return parsed;
}

// The mounted `smb-share:*` entries under `gvfsRoot`, each with its parsed server/share fields,
// sorted by directory name for deterministic iteration.
[[nodiscard]] std::vector<std::pair<QString, ParsedMountName>>
mountEntries(const QString& gvfsRoot) {
    std::vector<std::pair<QString, ParsedMountName>> entries;
    if (gvfsRoot.isEmpty())
        return entries;
    const QDir root(gvfsRoot);
    if (!root.exists())
        return entries;
    for (const auto& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (auto parsed = parseMountName(name))
            entries.emplace_back(name, std::move(*parsed));
    return entries;
}

} // namespace

QString defaultGvfsRoot() {
    const auto runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDir.isEmpty())
        return QDir(runtimeDir).filePath(QStringLiteral("gvfs"));
#if defined(Q_OS_UNIX)
    return QStringLiteral("/run/user/%1/gvfs").arg(static_cast<qulonglong>(getuid()));
#else
    return {};
#endif
}

std::optional<QString> localPathForUrl(const QUrl& url, const QString& gvfsRoot) {
    if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0)
        return url.toLocalFile();
    const auto parsed = parseSmbUrl(url);
    if (!parsed)
        return std::nullopt;
    const QDir root(gvfsRoot);
    for (const auto& [name, mount] : mountEntries(gvfsRoot)) {
        if (mount.server.compare(parsed->host, Qt::CaseInsensitive) != 0 ||
            mount.share != parsed->share)
            continue;
        const auto mountPath = root.filePath(name);
        return parsed->rest.isEmpty() ? mountPath : mountPath + QLatin1Char('/') + parsed->rest;
    }
    return std::nullopt;
}

QList<QUrl> mountedNetworkShares(const QString& gvfsRoot) {
    QList<QUrl> shares;
    const QDir root(gvfsRoot);
    for (const auto& [name, mount] : mountEntries(gvfsRoot)) {
        (void)mount;
        shares.push_back(QUrl::fromLocalFile(root.filePath(name)));
    }
    return shares;
}

std::optional<QString> networkShareLabel(const QString& mountDirectoryName) {
    const auto parsed = parseMountName(mountDirectoryName);
    if (!parsed)
        return std::nullopt;
    return parsed->server + QLatin1Char('/') + parsed->share;
}

std::optional<QString> smbShareLabel(const QUrl& url) {
    const auto parsed = parseSmbUrl(url);
    if (!parsed)
        return std::nullopt;
    return parsed->host + QLatin1Char('/') + parsed->share;
}

void configureFileDialogSidebar(QFileDialog& dialog, const QString& gvfsRoot) {
    const auto shares = mountedNetworkShares(gvfsRoot);
    // Qt's widget dialog keeps its sidebar between runs, so a share added while mounted would
    // stay listed after it is gone. Drop every share entry that is not mounted now (by its mount
    // directory name alone: no file system call on a possibly unreachable share), keep every other
    // place, then add the mounted shares once each.
    QList<QUrl> urls;
    for (const auto& url : dialog.sidebarUrls()) {
        const bool share =
            url.isLocalFile() && networkShareLabel(QFileInfo(url.toLocalFile()).fileName());
        if ((!share || shares.contains(url)) && !urls.contains(url))
            urls.push_back(url);
    }
    for (const auto& share : shares)
        if (!urls.contains(share))
            urls.push_back(share);
    if (urls != dialog.sidebarUrls())
        dialog.setSidebarUrls(urls);
}

} // namespace bloom::ui::kit
