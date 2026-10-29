# UI Feedback: Sounds And Notifications

Status: current. Owner: `src/ui` (`ui_feedback`, `ui_sound_engine`, `interaction_sounds`,
`system_notifier`). The user-facing behaviour and the event audit are in
[`ux/sound.md`](../ux/sound.md).

## Rule

Callers say what happened, not what to play. The application owns one `ui::UiFeedback` and hands
it, explicitly, to the few places that report a result. There is no global instance, service
locator or event bus. Feedback is session preference state (`ApplicationPreferences`), never project
state, and it has no influence on evaluation or rendering.

## Pieces

| Piece | Role |
| --- | --- |
| `FeedbackEvent` | A meaning: `RenderSucceeded`, `RenderFailed`, `ImportFinished`, `ImportFailed`, `Warning`, `Refused` (the Event category) and `ControlPress`, `Select`, `ToggleOn`, `ToggleOff`, `ModalOpen`, `ModalClose`, `ToolSwitch`, `DragStart`, `Drop`, `Disabled` (the Interaction category). One table maps each to a bundled sound and a gain trim. |
| `UiFeedback` | Applies the Preferences switches (master, volume, Event, Interaction), the 80 ms same-sound repeat limit and the interaction suppression, then enqueues on a `SoundPlayer`. For a render result it also posts a `Notifier` notification when Bloom is not the active application. Pure C++ over two seams and an injectable clock and focus probe, so it is tested without a device or a window system. |
| `SoundPlayer` | The seam to audio. `UiSoundEngine` is the real one. |
| `UiSoundEngine` | A separate small audio output over a `media::audio::playback::Backend` (miniaudio in the application). It never touches the transport `AudioEngine`, so UI sounds cannot disturb timeline or preview audio and do not take over a device (shared output). |
| `Notifier` | The seam to operating-system notifications; `makeSystemNotifier()` is the platform implementation. |
| `InteractionSounds` | An application event filter that connects the kit controls' own signals to interaction events, so the kit stays free of any sound dependency. |

## Threading

- `start()` returns at once. One worker thread reads each bundled WAV from the Qt resources and
  decodes it to float. The decoded sounds stay resident.
- The output device is opened lazily by that worker on the first `play()` and closed again after
  about 3 s without activity, so Bloom holds no output open while silent (and an exclusive ALSA
  device is only touched briefly). A later `play()` reopens it. The UI thread never reads, decodes,
  opens or closes a device.
- `play()` writes a small request into a fixed single-producer single-consumer ring and returns.
  It never blocks, allocates or locks. A full ring drops the cue.
- The device callback drains the ring into eight fixed voices and mixes mono into both channels. It
  does not allocate or lock.
- A device that cannot be opened leaves the engine `Unavailable`: silent, with one diagnostic on
  standard error and no dialog.
- Shutdown: `ApplicationShutdownCoordinator::shutdownStarted` calls `UiFeedback::shutdown()`, which
  stops the worker and the device; the destructor does the same again safely. A running worker
  checks the stop flag between sounds.

## Suppression

Interaction cues are muted during playback (`PlaybackController` state) and while a mouse press that
began on something other than a control is held (scrubbing, dragging a layer, a marquee). The drag
enter and drop cues are exempt: they report that gesture. Event cues are never suppressed.

## Calling it

- Result events: `uiFeedback.announceExport(outcome, message)` for every `FrameExportController`
  outcome (frame, range and composition export all emit `exportFinished`), `announceRender(ok,
  detail)` for any other render-like job, and `play(event)` for the rest. The Engine's Export queue
  calls `announceRender` when a job finishes; it needs no other dependency.
- Interaction events come from `InteractionSounds`. A new kit control that should cue adds one case
  to `InteractionSounds::hook`.

## Notifications

`Notifier::post` never blocks. Linux uses `org.freedesktop.Notifications` over QtDBus when the Qt
build has it (a click on the default action raises Bloom) and the tray message otherwise. macOS
uses `UNUserNotificationCenter` from `system_notifier_macos.mm` when Bloom runs from a bundle and the
tray message otherwise. Windows uses `QSystemTrayIcon::showMessage`. Where no surface exists,
nothing is posted and a line is logged. The Preferences switch and the active-application test are
both applied in `UiFeedback`, not in the platform code.

## Assets

13 processed sounds from SND01 "sine" under `src/ui/sound/assets`, embedded with `qt_add_resources`
(`/bloom/sound/<name>.wav`), mono 16-bit 48 kHz PCM, with a `provenance.md`. Credit and terms are in
`THIRD_PARTY_NOTICES.md` and the Open Source Licenses window.
