#include <bloom/ui/ui_feedback.hpp>

#include <bloom/ui/application_preferences.hpp>

#include <QCoreApplication>
#include <QGuiApplication>

#include <algorithm>

namespace bloom::ui {

namespace {

struct EventRow final {
    const char* name;
    FeedbackCategory category;
    FeedbackSound sound;
    float gain;
};

// Indexed by FeedbackEvent. The Warning and failure events share the "caution" sound; a refused
// command and a pressed disabled control share the "disabled" sound, the latter at half level.
constexpr EventRow kRows[] = {
    {"RenderSucceeded", FeedbackCategory::Event, FeedbackSound::RenderSucceeded, 1.0F},
    {"RenderFailed", FeedbackCategory::Event, FeedbackSound::RenderFailed, 1.0F},
    {"ImportFinished", FeedbackCategory::Event, FeedbackSound::ImportFinished, 1.0F},
    {"ImportFailed", FeedbackCategory::Event, FeedbackSound::RenderFailed, 1.0F},
    {"Warning", FeedbackCategory::Event, FeedbackSound::RenderFailed, 1.0F},
    {"Refused", FeedbackCategory::Event, FeedbackSound::Refused, 1.0F},
    {"ControlPress", FeedbackCategory::Interaction, FeedbackSound::Button, 1.0F},
    {"Select", FeedbackCategory::Interaction, FeedbackSound::Select, 1.0F},
    {"ToggleOn", FeedbackCategory::Interaction, FeedbackSound::ToggleOn, 1.0F},
    {"ToggleOff", FeedbackCategory::Interaction, FeedbackSound::ToggleOff, 1.0F},
    {"ModalOpen", FeedbackCategory::Interaction, FeedbackSound::ModalOpen, 1.0F},
    {"ModalClose", FeedbackCategory::Interaction, FeedbackSound::ModalClose, 1.0F},
    {"ToolSwitch", FeedbackCategory::Interaction, FeedbackSound::ToolSwitch, 1.0F},
    {"DragStart", FeedbackCategory::Interaction, FeedbackSound::DragStart, 1.0F},
    {"Drop", FeedbackCategory::Interaction, FeedbackSound::Drop, 1.0F},
    {"Disabled", FeedbackCategory::Interaction, FeedbackSound::Refused, 0.5F},
};
static_assert(std::size(kRows) == static_cast<std::size_t>(FeedbackEvent::Disabled) + 1);

const EventRow& row(const FeedbackEvent event) noexcept {
    return kRows[static_cast<std::size_t>(event)];
}

constexpr const char* kSoundNames[] = {
    "render_succeeded", "render_failed", "import_finished", "refused",    "button",
    "select",           "toggle_on",     "toggle_off",      "modal_open", "modal_close",
    "tool_switch",      "drag_start",    "drop",
};
static_assert(std::size(kSoundNames) == kFeedbackSoundCount);

bool defaultActive() {
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
}

} // namespace

FeedbackCategory feedbackCategory(const FeedbackEvent event) noexcept { return row(event).category; }
FeedbackSound feedbackSound(const FeedbackEvent event) noexcept { return row(event).sound; }
float feedbackGain(const FeedbackEvent event) noexcept { return row(event).gain; }
const char* feedbackEventName(const FeedbackEvent event) noexcept { return row(event).name; }
const char* feedbackSoundName(const FeedbackSound sound) noexcept {
    return kSoundNames[static_cast<std::size_t>(sound)];
}

FeedbackSettings feedbackSettingsFrom(const ApplicationPreferences& preferences) {
    return FeedbackSettings{preferences.soundsEnabled, preferences.soundVolumePercent,
                            preferences.eventSoundsEnabled, preferences.interactionSoundsEnabled,
                            preferences.renderNotificationsEnabled};
}

std::optional<FeedbackEvent> importFeedbackEvent(const bool cancelled, const bool discarded,
                                                 const std::size_t imported,
                                                 const std::size_t skipped, const bool failure) {
    if (cancelled || discarded) {
        return std::nullopt;
    }
    if (failure || (imported == 0 && skipped > 0)) {
        return FeedbackEvent::ImportFailed;
    }
    if (skipped > 0) {
        return FeedbackEvent::Warning;
    }
    if (imported > 0) {
        return FeedbackEvent::ImportFinished;
    }
    return std::nullopt;
}

UiFeedback::UiFeedback(std::unique_ptr<SoundPlayer> player, std::unique_ptr<Notifier> notifier,
                       ActiveProbe isActive, Clock clock)
    : player_(std::move(player)), notifier_(std::move(notifier)),
      isActive_(isActive ? std::move(isActive) : ActiveProbe{defaultActive}),
      clock_(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}) {}

UiFeedback::~UiFeedback() { shutdown(); }

void UiFeedback::setSettings(const FeedbackSettings& settings) noexcept {
    settings_ = settings;
    settings_.volumePercent = std::clamp(settings_.volumePercent, 0, 100);
}

void UiFeedback::setInteractionSuppressed(const Suppression reason, const bool on) noexcept {
    const auto bit = static_cast<std::uint8_t>(reason);
    suppression_ = on ? static_cast<std::uint8_t>(suppression_ | bit)
                      : static_cast<std::uint8_t>(suppression_ & ~bit);
}

bool UiFeedback::wouldPlay(const FeedbackEvent event) const noexcept {
    if (shutDown_ || player_ == nullptr || !settings_.soundsEnabled ||
        settings_.volumePercent == 0) {
        return false;
    }
    if (row(event).category == FeedbackCategory::Event) {
        return settings_.eventSounds;
    }
    if (!settings_.interactionSounds) {
        return false;
    }
    return suppression_ == 0 || event == FeedbackEvent::DragStart || event == FeedbackEvent::Drop;
}

bool UiFeedback::enqueue(const FeedbackSound sound, const float gain) {
    const auto index = static_cast<std::size_t>(sound);
    const auto now = clock_();
    if (hasPlayed_[index] && now - lastPlayed_[index] < kMinimumRepeat) {
        return false;
    }
    hasPlayed_[index] = true;
    lastPlayed_[index] = now;
    player_->play(sound, gain);
    return true;
}

bool UiFeedback::play(const FeedbackEvent event) {
    if (!wouldPlay(event)) {
        return false;
    }
    return enqueue(row(event).sound,
                   row(event).gain * static_cast<float>(settings_.volumePercent) / 100.0F);
}

bool UiFeedback::playTest(const FeedbackSettings& draft) {
    if (shutDown_ || player_ == nullptr || !draft.soundsEnabled || draft.volumePercent <= 0) {
        return false;
    }
    // The test ignores the repeat window so that a second press is always heard.
    const auto percent = static_cast<float>(std::clamp(draft.volumePercent, 0, 100));
    hasPlayed_[static_cast<std::size_t>(FeedbackSound::RenderSucceeded)] = false;
    return enqueue(FeedbackSound::RenderSucceeded, percent / 100.0F);
}

void UiFeedback::announceRender(const bool succeeded, const QString& detail) {
    play(succeeded ? FeedbackEvent::RenderSucceeded : FeedbackEvent::RenderFailed);
    if (notifier_ == nullptr || !settings_.renderNotifications || isActive_()) {
        return;
    }
    notifier_->post(SystemNotification{
        succeeded ? QCoreApplication::translate("UiFeedback", "Render complete")
                  : QCoreApplication::translate("UiFeedback", "Render failed"),
        detail, !succeeded});
}

void UiFeedback::setNotificationActivationHandler(std::function<void()> handler) {
    if (notifier_ != nullptr) {
        notifier_->setActivationHandler(std::move(handler));
    }
}

void UiFeedback::shutdown() noexcept {
    shutDown_ = true;
    if (player_ != nullptr) {
        player_->shutdown();
    }
}

} // namespace bloom::ui
