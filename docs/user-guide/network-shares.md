# Import from network shares

Bloom can import media directly from a network (SMB) share once your operating system has it
mounted. Bloom never mounts, connects to, or authenticates a share itself — that stays your file
manager's job, exactly as it is for any other network drive.

## Linux: mount the share first, then import

On Linux, mount the share in your file manager (for example, GNOME Files' **Other Locations**, or
`Connect to Server…`) before importing. Bloom reads the GNOME/GVFS mount your file manager
creates under `$XDG_RUNTIME_DIR/gvfs` (typically `/run/user/<your user id>/gvfs`).

Once mounted, you can:

- **Drag files from your file manager onto the Assets panel.** A file manager hands Bloom an
  `smb://server/share/path` URL on drop; Bloom resolves it to the mounted share and imports it
  exactly as it would a local file.
- **Use File > Import, the Assets footer's Import button, or Relink**, and pick the file from the
  mounted share in the Open dialog's sidebar, which lists every currently mounted network share.
- **Open Project, Save Project As, and Export Frame** also list mounted network shares in their
  dialog sidebar, so you can read from or write to a share without typing its full mounted path.

If you drag a file from a share your file manager has not connected yet, Bloom will not silently
do nothing: the status bar reports **"Connect to `<server>/<share>` in your file manager
first"**, so you know exactly what to do next. Connect to the share in your file manager, then
try the drop again.

## macOS and Windows

macOS mounts an SMB share under `/Volumes`, and Windows exposes one as a mapped drive letter or a
UNC path (`\\server\share`); both already appear as ordinary local paths once connected. Import,
Relink, and file drops onto Assets work the same way they do for any other local file — no
separate network-share support is needed on those platforms.

## Assets stay portable across the same machine

An asset imported from a network share resolves the same way any imported asset does: opening,
relinking, and thumbnailing all work as long as the share is still reachable through your file
manager's mount, using the same round trip through save and open as a locally imported asset. If
the file becomes unreachable (the share was disconnected, or the file was moved or removed), the
asset shows the usual missing-asset warning and can be relinked from Assets like any other.
