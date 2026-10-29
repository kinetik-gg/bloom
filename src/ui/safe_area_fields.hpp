#pragma once

// The Action Safe / Title Safe pair, one control for every place that edits a composition's safe
// areas: the Composition dialog's Advanced section and the Viewer's Custom Safe Areas dialog. Both
// show the stored percentages in the same KValueFields with the same range, precision and inline
// reason, so the two surfaces can never disagree about what a value means or which are refused.

#include <bloom/document/composition_settings.hpp>

#include <QString>

#include <optional>

class QWidget;

namespace bloom::ui {
namespace kit {
class KValueField;
} // namespace kit

struct SafeAreaFields final {
    kit::KValueField* action = nullptr;
    kit::KValueField* title = nullptr;

    // Two fields parented to `parent`, named `actionName` / `titleName` for automation.
    [[nodiscard]] static SafeAreaFields create(QWidget& parent, const QString& actionName,
                                               const QString& titleName);
    void setSettings(const document::SafeAreaSettings& settings);
    [[nodiscard]] document::SafeAreaSettings settings() const;
    // The reason the pair is refused (it belongs under Title), or nullopt.
    [[nodiscard]] std::optional<QString> issue() const;
};

} // namespace bloom::ui
