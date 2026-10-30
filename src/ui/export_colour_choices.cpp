#include "export_colour_choices.hpp"

#include "viewer_editor_internal.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/ui/composition_session.hpp>

#include <algorithm>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

QString ExportColourChoices::readable(const QString& name) {
    const std::string raw = name.toStdString();
    const std::string_view label = color::displayViewLabel(raw);
    return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
}

std::vector<QString> ExportColourChoices::displays() const {
    std::vector<QString> result;
    for (const auto& entry : displayViews)
        if (std::ranges::find(result, entry.display) == result.end())
            result.push_back(entry.display);
    return result;
}

std::vector<QString> ExportColourChoices::views(const QString& display) const {
    std::vector<QString> result;
    for (const auto& entry : displayViews)
        if ((display.isEmpty() || entry.display == display) &&
            std::ranges::find(result, entry.view) == result.end())
            result.push_back(entry.view);
    return result;
}

ExportColourChoices exportColourChoices(const CompositionSession& session) {
    ExportColourChoices choices;
    const auto resolved = viewer_detail::resolveViewerColorConfig(session);
    if (!resolved.has_value())
        return choices;
    choices.identity = text(std::string(resolved->configName())) + QLatin1Char('/') +
                       text(std::string(session.colorIntent().workingColorSpaceId));
    for (const auto& space : resolved->colorSpaces())
        choices.colourSpaces.emplace_back(text(space.id), text(space.id));
    for (const auto& entry : resolved->displays())
        choices.displayViews.push_back({text(entry.display), text(entry.view)});
    return choices;
}

} // namespace bloom::ui::exportpanel
