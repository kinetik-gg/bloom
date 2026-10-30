#pragma once

// Typed UI feedback: the one place that decides whether an application event is heard or shown.
//
// Callers emit semantic events (FeedbackEvent), never file names. The service maps an event to a
// sound and a category (Event or Interaction), checks the Preferences switches, rate-limits, and
// enqueues the sound on a SoundPlayer. It never decodes, reads a file, or touches an audio device
// itself; those live behind the SoundPlayer seam (ui_sound_engine.hpp), which tests replace with a
// fake. It also posts an operating-system notification for a render result when Bloom is not the
// active application, through the Notifier seam (system_notifier.hpp).
//
// The application owns one UiFeedback and passes it explicitly to the few call sites that need it
// (docs/architecture/ui-feedback.md). There is no global instance, locator or event bus.

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace bloom::ui {

struct ApplicationPreferences;

// A meaning, not a sound. Event kinds report a result the artist may be waiting for; Interaction
// kinds acknowledge a gesture and are off by default.
enum class FeedbackEvent : std::uint8_t {
    // Events.
    RenderSucceeded,
    RenderFailed,
    ImportFinished,
    ImportFailed,
    Warning,
    Refused,
    // Interactions.
    ControlPress,
    Select,
    ToggleOn,
    ToggleOff,
    ModalOpen,
    ModalClose,
    ToolSwitch,
    DragStart,
    Drop,
    Disabled,
};

enum class FeedbackCategory : std::uint8_t { Event, Interaction };

// The bundled, processed SND01 "sine" sounds (src/ui/sound/assets). Several events share one.
enum class FeedbackSound : std::uint8_t {
    RenderSucceeded,
    RenderFailed,
    ImportFinished,
    Refused,
    Button,
    Select,
    ToggleOn,
    ToggleOff,
    ModalOpen,
    ModalClose,
    ToolSwitch,
    DragStart,
    Drop,
};
inline constexpr std::size_t kFeedbackSoundCount = 13;

[[nodiscard]] FeedbackCategory feedbackCategory(FeedbackEvent event) noexcept;
[[nodiscard]] FeedbackSound feedbackSound(FeedbackEvent event) noexcept;
// A per-event trim on top of the sound's own level, for an event that reuses a louder sound.
[[nodiscard]] float feedbackGain(FeedbackEvent event) noexcept;
// The Qt resource file stem of a sound ("render_succeeded" for src/ui/sound/assets/...).
[[nodiscard]] const char* feedbackSoundName(FeedbackSound sound) noexcept;
[[nodiscard]] const char* feedbackEventName(FeedbackEvent event) noexcept;

// The Preferences state that governs feedback (application_preferences.hpp owns the stored form).
struct FeedbackSettings final {
    bool soundsEnabled = true;
    // 0 to 100; 100 plays the bundled level, which is already quiet.
    int volumePercent = 40;
    bool eventSounds = true;
    bool interactionSounds = false;
    bool renderNotifications = true;

    friend bool operator==(const FeedbackSettings&, const FeedbackSettings&) = default;
};

[[nodiscard]] FeedbackSettings feedbackSettingsFrom(const ApplicationPreferences& preferences);

// What a finished batch import is heard as: ImportFinished when everything landed, Warning when
// some items were skipped, ImportFailed when nothing was added, and nothing for a cancelled or
// discarded import. `failure` is true when the import reported a reason it added nothing.
[[nodiscard]] std::optional<FeedbackEvent> importFeedbackEvent(bool cancelled, bool discarded,
                                                               std::size_t imported,
                                                               std::size_t skipped, bool failure);

// Plays a decoded sound. play() only enqueues: it never blocks, decodes, or opens a device.
class SoundPlayer {
  public:
    virtual ~SoundPlayer() = default;
    virtual void play(FeedbackSound sound, float gain) noexcept = 0;
    // Releases the audio output. Idempotent; play() after it is a no-op.
    virtual void shutdown() noexcept = 0;
};

struct SystemNotification final {
    QString title;
    QString body;
    bool failure = false;
};

// Posts an operating-system notification. Implementations never block the caller.
class Notifier {
  public:
    virtual ~Notifier() = default;
    virtual void post(const SystemNotification& notification) = 0;
    // Called (on the UI thread) when the artist clicks a notification this notifier posted.
    virtual void setActivationHandler(std::function<void()> handler) = 0;
};

class UiFeedback final {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // True while Bloom is the active application.
    using ActiveProbe = std::function<bool()>;

    // The same sound is not enqueued twice within this window (an event followed at once by the
    // warning dialog that reports it is heard once).
    static constexpr std::chrono::milliseconds kMinimumRepeat{80};

    // Any argument may be empty: no player is silent, no notifier posts nothing, and the default
    // probe and clock are the application's own.
    explicit UiFeedback(std::unique_ptr<SoundPlayer> player,
                        std::unique_ptr<Notifier> notifier = nullptr, ActiveProbe isActive = {},
                        Clock clock = {});
    ~UiFeedback();
    UiFeedback(const UiFeedback&) = delete;
    UiFeedback& operator=(const UiFeedback&) = delete;

    void setSettings(const FeedbackSettings& settings) noexcept;
    [[nodiscard]] const FeedbackSettings& settings() const noexcept { return settings_; }

    // Plays `event` if the master switch, its category and the rate limit allow it. Returns whether
    // a sound was enqueued.
    bool play(FeedbackEvent event);
    // The settings-only part of play(), for callers that avoid building a cue that would be muted.
    [[nodiscard]] bool wouldPlay(FeedbackEvent event) const noexcept;

    // Interaction sounds are muted while the artist plays back or scrubs/drags a canvas. The drag
    // and drop cues themselves are never muted by a gesture: they are the gesture's feedback.
    enum class Suppression : std::uint8_t { Playback = 1, Gesture = 2 };
    void setInteractionSuppressed(Suppression reason, bool on) noexcept;
    [[nodiscard]] bool interactionSuppressed() const noexcept { return suppression_ != 0; }

    // A render or export finished. Plays RenderSucceeded or RenderFailed and, when the Preferences
    // switch is on and Bloom is not the active application, posts an operating-system notification.
    void announceRender(bool succeeded, const QString& detail);

    // Plays a bundled sample at `draft`'s volume regardless of the category switches (still
    // honouring its master switch), for the Preferences "Play test sound" button.
    bool playTest(const FeedbackSettings& draft);

    void setNotificationActivationHandler(std::function<void()> handler);
    // Releases the audio output. Safe to call more than once; the destructor calls it.
    void shutdown() noexcept;

  private:
    // `gain` is the final linear level (event trim times the volume).
    bool enqueue(FeedbackSound sound, float gain);

    std::unique_ptr<SoundPlayer> player_;
    std::unique_ptr<Notifier> notifier_;
    ActiveProbe isActive_;
    Clock clock_;
    FeedbackSettings settings_;
    std::uint8_t suppression_ = 0;
    bool shutDown_ = false;
    std::chrono::steady_clock::time_point lastPlayed_[kFeedbackSoundCount]{};
    bool hasPlayed_[kFeedbackSoundCount]{};
};

} // namespace bloom::ui
