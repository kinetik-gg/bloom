#pragma once

// Small helpers shared by the Export detail pane's translation units.

#include "export_panel_text.hpp"

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>

#include <QDir>
#include <QObject>
#include <QString>
#include <QUrl>

#include <array>
#include <utility>
#include <vector>

namespace bloom::ui::exportpanel::detail {

using document::ExportFormat;

[[nodiscard]] inline bool isImage(const ExportFormat format) {
    return format == ExportFormat::Png || format == ExportFormat::OpenExr ||
           format == ExportFormat::Tiff || format == ExportFormat::Dpx;
}
[[nodiscard]] inline bool isMovie(const ExportFormat format) {
    return !isImage(format) && format != ExportFormat::Wav;
}
[[nodiscard]] inline bool isH26x(const ExportFormat format) {
    return format == ExportFormat::H264Mov || format == ExportFormat::H264Mp4 ||
           format == ExportFormat::HevcMov || format == ExportFormat::HevcMp4;
}

using Choices = std::vector<std::pair<QString, QString>>;

// The engine's closed vocabulary for a token list (host::exportFormatDescription), so the panel
// never hard-codes tokens.
[[nodiscard]] inline Choices choicesOf(const std::vector<host::ExportFormatToken>& tokens) {
    Choices choices;
    for (const auto& token : tokens)
        choices.emplace_back(QString::fromUtf8(token.displayName.data(),
                                               static_cast<qsizetype>(token.displayName.size())),
                             QString::fromUtf8(token.token.data(),
                                               static_cast<qsizetype>(token.token.size())));
    return choices;
}

inline void fill(kit::KDropdown& dropdown, const Choices& choices) {
    dropdown.clearItems();
    for (const auto& [text, token] : choices)
        dropdown.addItem(text, token);
}

inline void select(kit::KDropdown& dropdown, const QString& token) {
    const int index = dropdown.findData(token);
    dropdown.setCurrentIndex(index >= 0 ? index : 0);
}

[[nodiscard]] inline std::array<std::uint8_t, 3> rgbOf(const QColor& color) {
    return {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
            static_cast<std::uint8_t>(color.blue())};
}

[[nodiscard]] inline QString displayPath(const document::ExportDestination& destination) {
    const auto text = qs(destination.path);
    if (destination.portability == "external") {
        const QUrl url(text);
        if (url.isLocalFile())
            return QDir::toNativeSeparators(url.toLocalFile());
    }
    return text;
}

template <typename Widget> inline Widget* named(Widget* widget, const QString& objectName,
                                         const QString& accessible = {}) {
    widget->setObjectName(objectName);
    widget->setAccessibleName(accessible.isEmpty() ? objectName : accessible);
    return widget;
}

[[nodiscard]] inline kit::KLabel* reasonLabel(QWidget* parent, const QString& objectName,
                                       const kit::Color tone) {
    auto* label = new kit::KLabel(parent);
    label->setObjectName(objectName);
    label->setTypeRole(kit::TypeRole::UiSmall);
    label->setTone(tone);
    label->setWordWrap(true);
    return label;
}

} // namespace bloom::ui::exportpanel::detail
