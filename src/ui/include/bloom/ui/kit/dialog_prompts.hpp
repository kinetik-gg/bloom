#pragma once

#include <bloom/ui/kit/dialog.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace bloom::ui::kit {

// What a prompt is about, which picks its glyph: a question, a warning before something
// destructive, an error that already happened, or plain information.
enum class KPromptTone : std::uint8_t { Question, Warning, Error, Info };

// A confirmation. The heading names the subject and the action ("Save changes to “Shot 12”
// before opening another project?"); the detail says what follows. Each non-empty button text adds
// that button; a prompt with only acceptText is an error/info notice with one OK.
struct KConfirmSpec final {
    QString title;
    QString objectName;
    KPromptTone tone = KPromptTone::Question;
    QString heading;
    QString detail;
    // A tooltip on the detail for what people rarely need on screen (a digest, a preset id).
    QString detailToolTip;
    // Long read-only text (a licence) in a scroll area at most DiagnosticHeight tall.
    QString document;
    QString acceptText;
    QString destructiveText;
    QString alternateText;
    QString rejectText;
    // The button Return presses. A destructive prompt names the safe action (Reject).
    KDialog::Role defaultRole = KDialog::Role::Accept;
};

// Builds the confirmation, parented to `owner`'s window. Exposed so a caller can adjust it.
[[nodiscard]] KDialog* buildConfirm(QWidget& owner, const KConfirmSpec& spec);
// Synchronous: for a caller that must return the answer (documented at each call site).
[[nodiscard]] KDialog::Role confirm(QWidget& owner, const KConfirmSpec& spec);
// Asynchronous, window-modal: `done` receives the answer; never blocks the caller.
void confirm(QWidget& owner, const KConfirmSpec& spec, std::function<void(KDialog::Role)> done);
// An error or information notice with one OK, asynchronous.
void inform(QWidget& owner, KPromptTone tone, const QString& title, const QString& heading,
            const QString& detail = {}, const QString& objectName = {});

// One line of text. `validate` runs on every edit and returns the reason the text is refused,
// shown inside the prompt under the field; OK stays disabled meanwhile, so a refused name never
// closes and reopens the prompt.
struct KTextPromptSpec final {
    QString title;
    QString objectName;
    QString label;
    QString text;
    QString placeholder;
    QString acceptText;
    std::function<std::optional<QString>(const QString&)> validate;
};
[[nodiscard]] KDialog* buildTextPrompt(QWidget& owner, const KTextPromptSpec& spec);
[[nodiscard]] std::optional<QString> promptText(QWidget& owner, const KTextPromptSpec& spec);
void promptText(QWidget& owner, const KTextPromptSpec& spec,
                std::function<void(std::optional<QString>)> done);

// An inclusive integer range. `addRows` may add rows above the range (an export destination);
// `validate` may refuse something beyond the built-in first-after-last rule.
struct KRangePromptSpec final {
    QString title;
    QString objectName;
    QString firstLabel;
    QString lastLabel;
    QString acceptText;
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    // The reason shown under Last when it comes before First.
    QString orderReason;
    std::function<void(KDialog&, KForm&)> addRows;
    std::function<std::optional<KRefusal>()> validate;
};
struct KRange final {
    std::int64_t first = 0;
    std::int64_t last = 0;
};
[[nodiscard]] KDialog* buildRangePrompt(QWidget& owner, const KRangePromptSpec& spec);
// The range a built range prompt shows now.
[[nodiscard]] KRange rangeOf(const KDialog& prompt);
[[nodiscard]] std::optional<KRange> promptRange(QWidget& owner, const KRangePromptSpec& spec);

} // namespace bloom::ui::kit
