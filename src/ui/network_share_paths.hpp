#pragma once
#include <QList>
#include <QString>
#include <QUrl>
#include <optional>

class QFileDialog;

namespace bloom::ui {

// This workstation's GVFS mount root for the current session: `$XDG_RUNTIME_DIR/gvfs`, falling
// back to `/run/user/<uid>/gvfs` when `XDG_RUNTIME_DIR` is unset. GVFS network mounts are a
// GNOME/Linux desktop convention; on macOS and Windows this returns an empty string and every
// function below that takes a `gvfsRoot` degrades to its "no shares" behaviour, which is correct
// there: a mounted SMB share already appears as an ordinary local path (a `/Volumes` entry, or a
// mapped drive letter) that `file://` URLs and the platform's native Open dialog already reach
// without this module's help.
[[nodiscard]] QString defaultGvfsRoot();

// Resolves a dropped or otherwise supplied URL to a local filesystem path usable with
// `std::filesystem`/`QFile`. `file://` URLs decode directly through `QUrl::toLocalFile()`.
// `smb://host/share/rest` URLs resolve through the `gvfsRoot` directory named `smb-share:...`
// that carries a `server=` field matching `host` (case-insensitive; `host` may be an IP or a
// name) and a `share=` field matching `share` -- the mount directory name may also carry
// `domain=`/`user=` fields, in any order. The match is joined with the URL's percent-decoded
// remainder. Any other scheme, or an `smb://` URL naming a share with no matching mount, returns
// `std::nullopt`.
[[nodiscard]] std::optional<QString> localPathForUrl(const QUrl& url,
                                                     const QString& gvfsRoot = defaultGvfsRoot());

// Every mounted `smb-share:*` GVFS directory under `gvfsRoot`, each as a `file://` URL naming
// that mount point -- the same directories a file manager shows as `host/share`. Empty when
// `gvfsRoot` does not exist or has no such mounts.
[[nodiscard]] QList<QUrl> mountedNetworkShares(const QString& gvfsRoot = defaultGvfsRoot());

// The `host/share` an `smb://` URL names, independent of whether that share is currently
// mounted. Used to compose a "Connect to <host>/<share> in your file manager first" notice for a
// drop the mapping above could not resolve. `std::nullopt` for a non-`smb` URL or one with no
// share segment.
[[nodiscard]] std::optional<QString> smbShareLabel(const QUrl& url);

// Adds every entry from `mountedNetworkShares(gvfsRoot)` to `dialog`'s sidebar, alongside its
// existing entries, so a non-native (Qt widget) file dialog can reach a mounted share; native
// platform dialogs own their own sidebar and ignore this. Safe to call unconditionally before
// `exec()`.
void configureFileDialogSidebar(QFileDialog& dialog, const QString& gvfsRoot = defaultGvfsRoot());

} // namespace bloom::ui
