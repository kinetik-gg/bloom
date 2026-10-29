# Sound Feedback And Notifications

Status: current. Architecture: [`architecture/ui-feedback.md`](../architecture/ui-feedback.md).
Settings: [`user-guide/preferences.md`](../user-guide/preferences.md).

## Principles

- Sound reports a result the artist may be waiting for. It does not decorate every gesture.
- **Event sounds are on by default** (render complete and failed, import results, warnings and
  refused commands). **Interaction sounds are off by default** (switches, choices, dialogs, tools,
  drag and drop) and are an opt-in in Preferences.
- Sounds are quiet. The bundled set is level-matched well below full scale and the default volume is
  40%.
- Never during scrubbing, and no interaction sound during playback. Playback itself is silent apart
  from the timeline audio; a render or import result is still heard.
- No hover sounds, no typing sounds, no sound for selection or for every keystroke.
- A missing audio device is silent and not an error. Nothing here ever shows a dialog.
- The same sound is not played twice within 80 ms: an error dialog that follows a failed render is
  heard once.

## Render notifications

When a render or export finishes or fails and Bloom is **not the active application**, the operating
system shows a notification ("Render complete" or "Render failed" with the message). Clicking it
brings Bloom forward where the platform supports that. The sound plays in either case if Event
sounds are on; the notification follows its own Preferences switch.

| Platform | Mechanism | Status |
| --- | --- | --- |
| Linux | `org.freedesktop.Notifications` over QtDBus (a click raises Bloom); the tray message without QtDBus or a session bus | Verified: builds with QtDBus; the notifier logic is covered by tests with a fake notifier |
| macOS | `UNUserNotificationCenter` from a bundle; the tray message from an unbundled binary | Written to the platform API, not built or run on macOS |
| Windows | `QSystemTrayIcon::showMessage` (the supported fallback); a click raises Bloom | Not built or run on Windows |

## Sounds

UI sounds: SND01 "sine" by Yasuhiro Tsuchiya, from SND (snd.dev) by Dentsu Inc. CX Creative Center.
The credit is in `THIRD_PARTY_NOTICES.md` and the Open Source Licenses window; the selection,
processing and digests are in `src/ui/sound/assets/provenance.md`. Bloom bundles 13 processed
sounds, not the pack.

| Sound | From SND01 | Used by |
| --- | --- | --- |
| `render_succeeded` | celebration ("Check me all") | RenderSucceeded; the Preferences test sound |
| `render_failed` | caution | RenderFailed, ImportFailed, Warning |
| `import_finished` | notification | ImportFinished |
| `refused` | disabled | Refused; Disabled at half level |
| `button` | button | ControlPress |
| `select` | select | Select |
| `toggle_on`, `toggle_off` | toggle_on, toggle_off | ToggleOn, ToggleOff |
| `modal_open`, `modal_close` | transition_up, transition_down | ModalOpen, ModalClose |
| `tool_switch` | tap_01 | ToolSwitch |
| `drag_start` | swipe_01 | DragStart |
| `drop` | tap_03 | Drop |

## Audit of user-facing events

Category **E** is an event sound (default on); **I** is an interaction sound (default off).
"Silent" means the event was considered and deliberately has no sound.

| Event | Where it fires | Cat. | Sound | Default |
| --- | --- | --- | --- | --- |
| Frame export finished | `FrameExportController::exportFinished` (`frame_export_controller.cpp`), wired in `apps/bloom/main.cpp` | E | render_succeeded + notification when inactive | On |
| Range (sequence) export finished | same signal, `frame_export_controller_composition.cpp` and the range driver | E | render_succeeded + notification | On |
| Composition (video) export finished | same signal (`frame_export_controller_composition.cpp`) | E | render_succeeded + notification | On |
| Export published with a warning | same signal, `PublishedWithWarning` | E | render_succeeded + notification | On |
| Export failed, refused or not approvable (including verification failures) | same signal, `Failed`, `Refused`, `NotApprovable` | E | render_failed + notification; the error dialog adds no second sound within 80 ms | On |
| Export cancelled | same signal, `Cancelled` | none | Silent: the artist asked for it | n/a |
| Engine Export queue job finished or failed | `UiFeedback::announceRender` (called by the queue) | E | render_succeeded or render_failed + notification | On |
| RAM preview cache complete | `RamPreviewController::cachingFinished` | none | Silent: it refills after most edits and would sound constantly; autoplay is the cue | n/a |
| Import finished | `AssetController::importFinished` (`asset_controller.cpp`) | E | import_finished | On |
| Import partial (some items skipped) | same signal | E | render_failed (caution) | On |
| Import failed (nothing added) | same signal | E | render_failed (caution) | On |
| Import cancelled or discarded | same signal | none | Silent | n/a |
| Project save failed | `ProjectHost::saveFinished` inform dialog (`main_window.cpp`) | E | render_failed via the Error prompt | On |
| Project open or copy failed | `openFinished`, `copyFinished` inform dialogs (`main_window.cpp`) | E | render_failed via the Error prompt | On |
| Recover offered | none: Bloom has no recovery prompt yet | n/a | n/a | n/a |
| Command refused | `CompositionSession::commandRejected` (`composition_session.cpp`), wired in `apps/bloom/main.cpp` | E | refused | On |
| Error dialog (any kit prompt with Error tone) | `kit::inform` / `kit::buildConfirm` (`kit/dialog_prompts.cpp`), seen by `InteractionSounds` | E | render_failed | On |
| Confirmation, Warning-tone and Info dialogs | same | I | modal_open (see Modal open) | Off |
| Modal dialog opened | `QDialog` show (`interaction_sounds.cpp`) | I | modal_open | Off |
| Modal dialog closed | `QDialog` hide | I | modal_close | Off |
| Pressed a disabled button or control | mouse press on a disabled kit control (`interaction_sounds.cpp`) | I | refused at half level | Off |
| Button press | `KButton::clicked`, `KIconButton::clicked` (not inside a dialog; its own open and close cue instead) | I | button | Off |
| Switch, checkbox or icon toggle changed by the user | `KSwitch`, `KCheckBox`, `KIconToggle::toggled` after a press | I | toggle_on or toggle_off | Off |
| Programmatic toggle (restore, reset, sync) | same signal without a press | none | Silent | n/a |
| Dropdown item chosen | `KDropdown::activated` | I | select | Off |
| Radio group or segmented control changed by the user | `KRadioGroup::currentIndexChanged` during a press or key | I | select | Off |
| Tool switched | a `KIconToggle` in the contextual bar tool group (`contextBarTools`) checked | I | tool_switch | Off |
| Drag started (Assets to Timeline, files) | `QEvent::DragEnter` the first time (`interaction_sounds.cpp`) | I | drag_start | Off |
| Drop | `QEvent::Drop` (`asset_drop.cpp`, `layer_drop.cpp` targets) | I | drop | Off |
| Scrub, playhead drag, canvas drag, marquee | timeline, viewer | none | Silent, and mutes interaction cues until release | n/a |
| Playback start, stop, loop | `PlaybackController::stateChanged` | none | Silent; mutes interaction cues while playing | n/a |
| Menu open, menu item chosen, hover, tooltip | menus | none | Silent | n/a |
| Typing, value-field edit, text entry | fields | none | Silent | n/a |
| Selection changes | session | none | Silent | n/a |
| Loading or progress loop | task progress | none | Not bundled: no task is long enough to benefit, and a loop contradicts the quiet default | n/a |
| Script errors | script panel | none | Silent for now: they are shown inline in the panel | n/a |

## Not covered

- Per-sound customisation, other packs and custom files.
- Sound for panel focus, layout changes or notifications from other applications.
