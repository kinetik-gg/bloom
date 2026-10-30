#pragma once

// The H.264 encoder remedy as its own flow: ask for the artist's consent to Cisco's OpenH264 licence,
// install it, or locate a copy they downloaded. The Export panel opens it for an
// EncoderNotInstalled preset (ExportRemedy::InstallOpenH264 / LocateOpenH264); the old export
// dialog uses the same two prompts.

#include <QString>

#include <optional>

class QWidget;

namespace bloom::ui {

class H264EncoderService;
namespace kit {
class KDialog;
}

// The licence prompt, synchronous over `owner`. True when the artist accepts and installation may
// begin.
[[nodiscard]] bool confirmOpenH264Install(QWidget& owner);
// The file chooser for a downloaded OpenH264 library; nullopt when cancelled.
[[nodiscard]] std::optional<QString> chooseOpenH264File(QWidget& owner);

// A dialog showing the encoder's state with Install and Locate actions and a busy bar while one
// runs. Window-modal and asynchronous through KDialog::open(); `service` outlives it.
[[nodiscard]] kit::KDialog* buildH264EncoderDialog(QWidget& owner, H264EncoderService& service);
void showH264EncoderFlow(QWidget& owner, H264EncoderService& service);

} // namespace bloom::ui
