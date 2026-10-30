#pragma once

// The colour spaces, displays and views an export output may name, read from the project's colour
// configuration. The Export panel offers these as dropdowns: a typed name that matches nothing in
// the configuration would be a blocking problem found only at Start.

#include <QString>

#include <utility>
#include <vector>

namespace bloom::ui {
class CompositionSession;
namespace exportpanel {

struct ExportColourChoices final {
    struct DisplayView final {
        QString display; // the configuration's own name (stored in the output's settings)
        QString view;
    };
    // (name an artist reads, configuration id) in the configuration's order.
    std::vector<std::pair<QString, QString>> colourSpaces;
    std::vector<DisplayView> displayViews;
    // Changes whenever the configuration the choices came from changes.
    QString identity;

    [[nodiscard]] static QString readable(const QString& name);
    [[nodiscard]] std::vector<QString> displays() const;
    // The views of `display`, or every view once when `display` is empty.
    [[nodiscard]] std::vector<QString> views(const QString& display) const;
};

[[nodiscard]] ExportColourChoices exportColourChoices(const CompositionSession& session);

} // namespace exportpanel
} // namespace bloom::ui
