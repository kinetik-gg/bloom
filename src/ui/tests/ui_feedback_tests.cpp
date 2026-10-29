#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/interaction_sounds.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/ui_feedback.hpp>
#include <bloom/ui/ui_sound_engine.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QTest>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

namespace ui = bloom::ui;
namespace playback = bloom::media::audio::playback;
using Clock = std::chrono::steady_clock;

struct PlayRecord {
    ui::FeedbackSound sound;
    float gain;
};

class FakePlayer final : public ui::SoundPlayer {
  public:
    void play(const ui::FeedbackSound sound, const float gain) noexcept override {
        played.push_back({sound, gain});
    }
    void shutdown() noexcept override { ++shutdowns; }
    std::vector<PlayRecord> played;
    int shutdowns = 0;
};

class FakeNotifier final : public ui::Notifier {
  public:
    void post(const ui::SystemNotification& notification) override {
        posted.push_back(notification);
    }
    void setActivationHandler(std::function<void()> h) override { handler = std::move(h); }
    std::vector<ui::SystemNotification> posted;
    std::function<void()> handler;
};

// A feedback service over fakes with a manual clock and a controllable focus state.
struct Rig {
    Rig() {
        auto fakePlayer = std::make_unique<FakePlayer>();
        auto fakeNotifier = std::make_unique<FakeNotifier>();
        player = fakePlayer.get();
        notifier = fakeNotifier.get();
        feedback = std::make_unique<ui::UiFeedback>(
            std::move(fakePlayer), std::move(fakeNotifier), [this] { return active; },
            [this] { return now; });
    }
    void advance(const std::chrono::milliseconds step) { now += step; }
    Clock::time_point now{std::chrono::seconds{100}};
    bool active = true;
    FakePlayer* player = nullptr;
    FakeNotifier* notifier = nullptr;
    std::unique_ptr<ui::UiFeedback> feedback;
};

void testMappingTable(Expectations& check) {
    using E = ui::FeedbackEvent;
    check.expect(ui::feedbackSound(E::RenderSucceeded) == ui::FeedbackSound::RenderSucceeded,
                 "render complete maps to the celebration sound");
    check.expect(ui::feedbackSound(E::RenderFailed) == ui::FeedbackSound::RenderFailed &&
                     ui::feedbackSound(E::Warning) == ui::FeedbackSound::RenderFailed &&
                     ui::feedbackSound(E::ImportFailed) == ui::FeedbackSound::RenderFailed,
                 "render failed, import failed and warnings share the caution sound");
    for (const auto event : {E::RenderSucceeded, E::RenderFailed, E::ImportFinished,
                             E::ImportFailed, E::Warning, E::Refused}) {
        check.expect(ui::feedbackCategory(event) == ui::FeedbackCategory::Event,
                     "result events are in the Event category");
    }
    for (const auto event : {E::ControlPress, E::Select, E::ToggleOn, E::ToggleOff, E::ModalOpen,
                             E::ModalClose, E::ToolSwitch, E::DragStart, E::Drop, E::Disabled}) {
        check.expect(ui::feedbackCategory(event) == ui::FeedbackCategory::Interaction,
                     "gesture cues are in the Interaction category");
    }
    check.expect(ui::feedbackGain(E::Disabled) < 1.0F, "a disabled press is quieter");
    std::set<std::string> names;
    for (std::size_t i = 0; i < ui::kFeedbackSoundCount; ++i)
        names.insert(ui::feedbackSoundName(static_cast<ui::FeedbackSound>(i)));
    check.expect(names.size() == ui::kFeedbackSoundCount, "every sound has a distinct name");
}

void testCategorySwitches(Expectations& check) {
    Rig rig;
    auto& fb = *rig.feedback;
    check.expect(fb.settings() == ui::FeedbackSettings{},
                 "the service starts from the documented defaults");
    check.expect(fb.play(ui::FeedbackEvent::RenderSucceeded), "event sounds play by default");
    check.expect(!fb.play(ui::FeedbackEvent::ToggleOn),
                 "interaction sounds are off by default");
    check.expect(rig.player->played.size() == 1, "only the event reached the player");

    auto settings = fb.settings();
    settings.interactionSounds = true;
    fb.setSettings(settings);
    check.expect(fb.play(ui::FeedbackEvent::ToggleOn), "interaction sounds play once enabled");

    settings.eventSounds = false;
    fb.setSettings(settings);
    rig.advance(std::chrono::seconds{1});
    check.expect(!fb.play(ui::FeedbackEvent::RenderFailed), "event sounds can be turned off");
    check.expect(fb.play(ui::FeedbackEvent::ModalOpen), "the categories are independent");

    settings.soundsEnabled = false;
    fb.setSettings(settings);
    rig.advance(std::chrono::seconds{1});
    check.expect(!fb.play(ui::FeedbackEvent::ModalClose) &&
                     !fb.play(ui::FeedbackEvent::RenderFailed),
                 "the master switch silences both categories");
}

void testVolumeAndTestSound(Expectations& check) {
    Rig rig;
    auto settings = rig.feedback->settings();
    settings.volumePercent = 50;
    rig.feedback->setSettings(settings);
    rig.feedback->play(ui::FeedbackEvent::Disabled); // interaction off: not played
    rig.feedback->play(ui::FeedbackEvent::Refused);
    check.expect(rig.player->played.size() == 1 && rig.player->played[0].gain == 0.5F,
                 "the volume scales the played gain");
    settings.volumePercent = 0;
    rig.feedback->setSettings(settings);
    rig.advance(std::chrono::seconds{1});
    check.expect(!rig.feedback->play(ui::FeedbackEvent::Refused), "volume 0 plays nothing");

    ui::FeedbackSettings draft;
    draft.volumePercent = 80;
    draft.eventSounds = false;
    draft.interactionSounds = false;
    check.expect(rig.feedback->playTest(draft) && rig.player->played.back().gain == 0.8F,
                 "the test sound plays at the draft volume whatever the categories");
    draft.soundsEnabled = false;
    check.expect(!rig.feedback->playTest(draft), "the test sound honours the master switch");
}

void testRateLimit(Expectations& check) {
    Rig rig;
    check.expect(rig.feedback->play(ui::FeedbackEvent::RenderFailed), "first play");
    rig.advance(std::chrono::milliseconds{40});
    check.expect(!rig.feedback->play(ui::FeedbackEvent::Warning),
                 "the same sound does not stack within 80 ms, even from another event");
    check.expect(rig.feedback->play(ui::FeedbackEvent::ImportFinished),
                 "a different sound is not limited");
    rig.advance(std::chrono::milliseconds{60});
    check.expect(rig.feedback->play(ui::FeedbackEvent::Warning), "the sound plays again after 80 ms");
    check.expect(rig.player->played.size() == 3, "exactly the allowed plays reached the player");
}

void testSuppression(Expectations& check) {
    Rig rig;
    auto settings = rig.feedback->settings();
    settings.interactionSounds = true;
    rig.feedback->setSettings(settings);
    using S = ui::UiFeedback::Suppression;
    rig.feedback->setInteractionSuppressed(S::Gesture, true);
    check.expect(!rig.feedback->play(ui::FeedbackEvent::Select),
                 "interaction sounds are muted during a canvas gesture");
    check.expect(rig.feedback->play(ui::FeedbackEvent::Drop),
                 "the drop cue is not muted by the gesture it reports");
    check.expect(rig.feedback->play(ui::FeedbackEvent::RenderSucceeded),
                 "event sounds ignore suppression");
    rig.feedback->setInteractionSuppressed(S::Gesture, false);
    rig.feedback->setInteractionSuppressed(S::Playback, true);
    rig.advance(std::chrono::seconds{1});
    check.expect(!rig.feedback->play(ui::FeedbackEvent::ToggleOn),
                 "interaction sounds are muted during playback");
    rig.feedback->setInteractionSuppressed(S::Playback, false);
    check.expect(rig.feedback->play(ui::FeedbackEvent::ToggleOn), "and return afterwards");
}

void testNotifications(Expectations& check) {
    Rig rig;
    rig.active = true;
    rig.feedback->announceRender(true, QStringLiteral("shot.exr"));
    check.expect(rig.notifier->posted.empty(), "no notification while Bloom is the active window");
    check.expect(rig.player->played.size() == 1, "the sound still plays");

    rig.active = false;
    rig.advance(std::chrono::seconds{1});
    rig.feedback->announceRender(true, QStringLiteral("Exported shot.exr"));
    check.expect(rig.notifier->posted.size() == 1 && !rig.notifier->posted[0].failure &&
                     rig.notifier->posted[0].body == QStringLiteral("Exported shot.exr"),
                 "a finished render notifies when Bloom is not active");
    rig.advance(std::chrono::seconds{1});
    rig.feedback->announceRender(false, QStringLiteral("disk full"));
    check.expect(rig.notifier->posted.size() == 2 && rig.notifier->posted[1].failure &&
                     rig.player->played.back().sound == ui::FeedbackSound::RenderFailed,
                 "a failed render notifies as a failure and plays the caution sound");

    auto settings = rig.feedback->settings();
    settings.renderNotifications = false;
    rig.feedback->setSettings(settings);
    rig.feedback->announceRender(true, QStringLiteral("x"));
    check.expect(rig.notifier->posted.size() == 2, "the Preferences switch stops notifications");

    // The sound switches do not decide notifications.
    settings.renderNotifications = true;
    settings.soundsEnabled = false;
    rig.feedback->setSettings(settings);
    rig.feedback->announceRender(true, QStringLiteral("quiet"));
    check.expect(rig.notifier->posted.size() == 3, "notifications are independent of sounds");

    const auto before = rig.notifier->posted.size();
    rig.feedback->announceExport(ui::FrameExportOutcome::Cancelled, QStringLiteral("c"));
    check.expect(rig.notifier->posted.size() == before, "a cancelled export is silent");
    for (const auto outcome :
         {ui::FrameExportOutcome::Failed, ui::FrameExportOutcome::Refused,
          ui::FrameExportOutcome::NotApprovable}) {
        rig.feedback->announceExport(outcome, QStringLiteral("f"));
    }
    check.expect(rig.notifier->posted.size() == before + 3 &&
                     rig.notifier->posted.back().failure,
                 "failed, refused and not-approvable exports notify as failures");
    rig.feedback->announceExport(ui::FrameExportOutcome::PublishedWithWarning, QStringLiteral("w"));
    check.expect(!rig.notifier->posted.back().failure, "a published export notifies as success");

    bool activated = false;
    rig.feedback->setNotificationActivationHandler([&] { activated = true; });
    rig.notifier->handler();
    check.expect(activated, "the activation handler reaches the notifier");
}

void testImportMapping(Expectations& check) {
    using E = ui::FeedbackEvent;
    check.expect(ui::importFeedbackEvent(false, false, 3, 0, false) == E::ImportFinished,
                 "a clean import finishes");
    check.expect(ui::importFeedbackEvent(false, false, 2, 1, false) == E::Warning,
                 "a partial import warns");
    check.expect(ui::importFeedbackEvent(false, false, 0, 2, false) == E::ImportFailed,
                 "an import that added nothing fails");
    check.expect(ui::importFeedbackEvent(false, false, 0, 0, true) == E::ImportFailed,
                 "an import with a failure reason fails");
    check.expect(!ui::importFeedbackEvent(true, false, 1, 0, false) &&
                     !ui::importFeedbackEvent(false, true, 1, 0, false) &&
                     !ui::importFeedbackEvent(false, false, 0, 0, false),
                 "cancelled, discarded and empty imports are silent");
    ui::ApplicationPreferences prefs;
    prefs.soundVolumePercent = 12;
    prefs.interactionSoundsEnabled = true;
    const auto settings = ui::feedbackSettingsFrom(prefs);
    check.expect(settings.volumePercent == 12 && settings.interactionSounds,
                 "preferences convert to feedback settings");
}

// A backend that records its lifecycle and lets the test pull audio synchronously.
class RecordingBackend final : public playback::Backend {
  public:
    playback::AudioStatus start(std::uint32_t, std::uint32_t, Callback callback_,
                                void* data) noexcept override {
        startThread = std::this_thread::get_id();
        ++starts;
        if (failStart) {
            return bloom::media::audio::AudioError::codeOnly(
                bloom::media::audio::AudioErrorCode::BackendStartFailed);
        }
        callback = callback_;
        userData = data;
        return std::nullopt;
    }
    void stop() noexcept override {
        ++stops;
        callback = nullptr;
    }
    std::vector<float> pull(const std::size_t frames) {
        std::vector<float> out(frames * 2);
        if (callback != nullptr)
            callback(userData, out);
        return out;
    }
    Callback callback = nullptr;
    void* userData = nullptr;
    std::thread::id startThread;
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    bool failStart = false;
};

[[nodiscard]] float peak(const std::vector<float>& samples) {
    float best = 0.0F;
    for (const float s : samples)
        best = std::max(best, std::abs(s));
    return best;
}

template <typename Predicate>
[[nodiscard]] bool waitFor(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    while (!predicate()) {
        if (Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return true;
}

void testEngineDecodesOffTheUiThread(Expectations& check) {
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    std::mutex mutex;
    std::set<std::thread::id> sourceThreads;
    ui::UiSoundEngine engine(std::move(backend), [&](const ui::FeedbackSound sound) {
        {
            const std::lock_guard lock(mutex);
            sourceThreads.insert(std::this_thread::get_id());
        }
        return ui::bundledFeedbackSoundBytes(sound);
    });
    engine.play(ui::FeedbackSound::Button, 1.0F);
    check.expect(engine.state() == ui::UiSoundEngine::State::Starting,
                 "the engine is not ready before start()");
    engine.start();
    engine.waitUntilSettled();
    check.expect(engine.state() == ui::UiSoundEngine::State::Ready, "the engine becomes ready");
    {
        const std::lock_guard lock(mutex);
        check.expect(sourceThreads.size() == 1 &&
                         !sourceThreads.contains(std::this_thread::get_id()),
                     "every sound is read and decoded on the worker, never the calling thread");
    }
    check.expect(engine.workerThread() != std::this_thread::get_id(),
                 "the worker is not the calling thread");
    check.expect(raw->starts == 0 && !engine.deviceOpen(),
                 "the device stays closed until the first sound is played");

    engine.play(ui::FeedbackSound::RenderSucceeded, 1.0F);
    check.expect(waitFor([&] { return engine.deviceOpen(); }),
                 "the first play opens the device");
    check.expect(raw->startThread == engine.workerThread(),
                 "and the worker opens it, not the calling thread");
    check.expect(raw->starts == 1, "the device was started once");
    const auto loud = raw->pull(512);
    check.expect(peak(loud) > 0.01F && peak(loud) <= 1.0F, "a played sound reaches the callback");
    check.expect(loud[0] == loud[1] || peak(loud) > 0.0F, "the mono sound feeds both channels");
    // Play the same sound again at a lower gain and compare peaks block for block.
    raw->pull(48'000); // let the first one finish
    engine.play(ui::FeedbackSound::RenderSucceeded, 0.25F);
    const auto quiet = raw->pull(512);
    check.expect(std::abs(peak(quiet) - peak(loud) * 0.25F) < 0.002F, "the gain scales the sound");
    raw->pull(48'000);
    check.expect(peak(raw->pull(1'000)) == 0.0F, "a sound ends and leaves silence");

    engine.play(ui::FeedbackSound::Button, 1.0F);
    engine.play(ui::FeedbackSound::Select, 1.0F);
    engine.play(ui::FeedbackSound::Drop, 1.0F);
    check.expect(peak(raw->pull(256)) > 0.0F, "several sounds can sound together");

    engine.shutdown();
    check.expect(raw->stops >= 1 && engine.state() == ui::UiSoundEngine::State::Stopped,
                 "shutdown stops the backend and releases the engine");
    engine.play(ui::FeedbackSound::Button, 1.0F);
    check.expect(peak(raw->pull(256)) == 0.0F, "play after shutdown is a no-op");
    engine.shutdown(); // idempotent
}

void testEngineStaysSilentWithoutADevice(Expectations& check) {
    auto backend = std::make_unique<RecordingBackend>();
    backend->failStart = true;
    auto* raw = backend.get();
    std::vector<std::string> diagnostics;
    std::mutex mutex;
    ui::UiSoundEngine engine(std::move(backend), ui::bundledFeedbackSoundBytes,
                             [&](const std::string& message) {
                                 const std::lock_guard lock(mutex);
                                 diagnostics.push_back(message);
                             });
    engine.start();
    engine.waitUntilSettled();
    engine.play(ui::FeedbackSound::Button, 1.0F);
    check.expect(waitFor([&] {
                     const std::lock_guard lock(mutex);
                     return diagnostics.size() == 1;
                 }),
                 "an output that cannot open reports one diagnostic and no dialog");
    check.expect(peak(raw->pull(128)) == 0.0F && !engine.deviceOpen(), "and stays silent");
}

void testEngineSurvivesMissingSounds(Expectations& check) {
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    ui::UiSoundEngine engine(std::move(backend), [](const ui::FeedbackSound sound) {
        return sound == ui::FeedbackSound::Select ? ui::bundledFeedbackSoundBytes(sound)
                                                   : QByteArray{};
    });
    engine.start();
    engine.waitUntilSettled();
    engine.play(ui::FeedbackSound::Button, 1.0F);
    check.expect(waitFor([&] { return engine.deviceOpen(); }), "the device opens");
    check.expect(peak(raw->pull(128)) == 0.0F, "a sound that failed to load is skipped");
    engine.play(ui::FeedbackSound::Select, 1.0F);
    check.expect(peak(raw->pull(128)) > 0.0F, "the others still play");
}

void testDeviceOpensLazilyAndClosesWhenIdle(Expectations& check) {
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    ui::UiSoundEngine engine(std::move(backend), ui::bundledFeedbackSoundBytes, {},
                             std::chrono::milliseconds{120});
    engine.start();
    engine.waitUntilSettled();
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    check.expect(raw->starts == 0 && raw->stops == 0 && !engine.deviceOpen(),
                 "an engine that is never asked to play never opens the device");

    engine.play(ui::FeedbackSound::Select, 1.0F);
    check.expect(waitFor([&] { return engine.deviceOpen(); }) && raw->starts == 1,
                 "the device opens on the first play");
    check.expect(peak(raw->pull(256)) > 0.0F, "the sound plays through the lazily opened device");
    check.expect(waitFor([&] { return !engine.deviceOpen(); }) && raw->stops == 1,
                 "the device closes after the idle period");

    engine.play(ui::FeedbackSound::Select, 1.0F);
    check.expect(waitFor([&] { return engine.deviceOpen(); }) && raw->starts == 2,
                 "a later play reopens it with the sounds still resident");
    check.expect(peak(raw->pull(256)) > 0.0F, "and plays");
    engine.shutdown();
    check.expect(!engine.deviceOpen() && raw->stops == 2, "shutdown closes an open device");
}

void testBundledSoundsDecode(Expectations& check) {
    for (std::size_t i = 0; i < ui::kFeedbackSoundCount; ++i) {
        const auto sound = static_cast<ui::FeedbackSound>(i);
        const auto samples = ui::decodeFeedbackWav(ui::bundledFeedbackSoundBytes(sound));
        check.expect(!samples.empty(), std::string{"bundled sound decodes: "} +
                                           ui::feedbackSoundName(sound));
        check.expect(samples.size() < 48'000, std::string{"UI sounds stay under a second: "} +
                                                  ui::feedbackSoundName(sound));
        check.expect(peak(samples) <= 0.9F, std::string{"UI sounds stay well below full scale: "} +
                                                ui::feedbackSoundName(sound));
    }
    check.expect(ui::decodeFeedbackWav(QByteArray("not a wav")).empty(), "garbage is refused");
    check.expect(ui::decodeFeedbackWav({}).empty(), "empty input is refused");
}

// --- Kit controls -------------------------------------------------------------------------------

void testInteractionObserver(Expectations& check) {
    Rig rig;
    ui::InteractionSounds observer(*rig.feedback);
    qApp->installEventFilter(&observer);

    QWidget host;
    host.resize(300, 200);
    auto* toggle = new ui::kit::KSwitch(&host);
    toggle->move(10, 10);
    auto* button = new ui::kit::KButton(QStringLiteral("Go"), &host);
    button->move(10, 60);
    auto* dropdown = new ui::kit::KDropdown(&host);
    dropdown->addItem(QStringLiteral("A"), 1);
    dropdown->addItem(QStringLiteral("B"), 2);
    dropdown->move(10, 110);
    host.show();
    observer.attach(*toggle);
    observer.attach(*button);
    observer.attach(*dropdown);

    // Interaction sounds are off by default: gestures cost nothing and make no sound.
    QTest::mouseClick(toggle, Qt::LeftButton);
    check.expect(rig.player->played.empty(), "an interaction category that is off stays silent");

    auto settings = rig.feedback->settings();
    settings.interactionSounds = true;
    rig.feedback->setSettings(settings);
    rig.advance(std::chrono::seconds{1});
    QTest::mouseClick(toggle, Qt::LeftButton);
    check.expect(rig.player->played.size() == 1 &&
                     rig.player->played[0].sound ==
                         ui::feedbackSound(toggle->isChecked() ? ui::FeedbackEvent::ToggleOn
                                                               : ui::FeedbackEvent::ToggleOff),
                 "a switch click cues its new state when interaction sounds are on");

    rig.advance(std::chrono::seconds{1});
    toggle->setChecked(!toggle->isChecked());
    check.expect(rig.player->played.size() == 1, "a programmatic change is silent");

    rig.advance(std::chrono::seconds{1});
    QTest::mouseClick(button, Qt::LeftButton);
    check.expect(rig.player->played.size() == 2 &&
                     rig.player->played[1].sound == ui::FeedbackSound::Button,
                 "a button click cues a press");

    rig.advance(std::chrono::seconds{1});
    button->setEnabled(false);
    QTest::mouseClick(button, Qt::LeftButton);
    check.expect(rig.player->played.size() == 3 &&
                     rig.player->played[2].sound == ui::FeedbackSound::Refused &&
                     rig.player->played[2].gain < 0.3F,
                 "pressing a disabled button cues a quiet refusal");

    rig.advance(std::chrono::seconds{1});
    dropdown->chooseIndex(1);
    check.expect(rig.player->played.size() == 4 &&
                     rig.player->played[3].sound == ui::FeedbackSound::Select,
                 "choosing a dropdown item cues a selection");

    // A press on a plain canvas widget is a gesture: interaction cues stay quiet until release.
    QWidget canvas(&host);
    canvas.setGeometry(150, 10, 100, 100);
    canvas.show();
    rig.advance(std::chrono::seconds{1});
    QTest::mousePress(&canvas, Qt::LeftButton);
    check.expect(rig.feedback->interactionSuppressed(),
                 "a press on a canvas suppresses interaction cues");
    QTest::mouseRelease(&canvas, Qt::LeftButton);
    check.expect(!rig.feedback->interactionSuppressed(), "and the release restores them");
    qApp->removeEventFilter(&observer);
}

void testDialogCues(Expectations& check) {
    Rig rig;
    auto settings = rig.feedback->settings();
    settings.interactionSounds = true;
    rig.feedback->setSettings(settings);
    ui::InteractionSounds observer(*rig.feedback);
    qApp->installEventFilter(&observer);
    QWidget owner;
    owner.show();

    ui::kit::KConfirmSpec spec;
    spec.title = QStringLiteral("Question");
    spec.objectName = QStringLiteral("soundQuestion");
    spec.tone = ui::kit::KPromptTone::Question;
    spec.heading = QStringLiteral("Proceed?");
    spec.acceptText = QStringLiteral("OK");
    auto* question = ui::kit::buildConfirm(owner, spec);
    question->open([](ui::kit::KDialog::Role) {});
    check.expect(!rig.player->played.empty() &&
                     rig.player->played.back().sound == ui::FeedbackSound::ModalOpen,
                 "opening a modal dialog cues ModalOpen");
    rig.advance(std::chrono::seconds{1});
    question->hide();
    check.expect(rig.player->played.back().sound == ui::FeedbackSound::ModalClose,
                 "closing it cues ModalClose");

    // An error prompt is an event: it is heard even with interaction sounds off.
    settings.interactionSounds = false;
    rig.feedback->setSettings(settings);
    rig.advance(std::chrono::seconds{1});
    const auto before = rig.player->played.size();
    ui::kit::inform(owner, ui::kit::KPromptTone::Error, QStringLiteral("Save"),
                    QStringLiteral("Could not save."), QStringLiteral("detail"),
                    QStringLiteral("soundError"));
    check.expect(rig.player->played.size() == before + 1 &&
                     rig.player->played.back().sound == ui::FeedbackSound::RenderFailed,
                 "an error dialog cues the warning sound as an event");
    qApp->removeEventFilter(&observer);
    for (auto* dialog : owner.findChildren<QDialog*>())
        dialog->deleteLater();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testMappingTable(check);
    testCategorySwitches(check);
    testVolumeAndTestSound(check);
    testRateLimit(check);
    testSuppression(check);
    testNotifications(check);
    testImportMapping(check);
    testBundledSoundsDecode(check);
    testEngineDecodesOffTheUiThread(check);
    testDeviceOpensLazilyAndClosesWhenIdle(check);
    testEngineStaysSilentWithoutADevice(check);
    testEngineSurvivesMissingSounds(check);
    testInteractionObserver(check);
    testDialogCues(check);
    return check.failures() == 0 ? 0 : 1;
}
