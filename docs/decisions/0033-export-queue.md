# ADR 0033: Export Queue And The Export Panel

Status: accepted (owner decisions of 2026-10-30; the supervisor may renumber the ADR and the schema
minor at integration)

Date: 2026-10-30

## Context

Exporting today is one modal flow per window: **File → Export Frame…**, **Export Frame Range…** and
**Export Composition…** open a dialog, run an analysis, stop on an approval prompt and then publish.
One export runs at a time, the settings are lost when it ends, and the orchestration lives in
`src/ui/frame_export_controller*.cpp`, a Qt class, so headless scripting, MCP and `bloom-cli` reach
the same pipeline through separate code.

The owner wants an Export panel with an export queue, like After Effects' Render Queue, and has
decided:

1. The queue (jobs, render settings and outputs) is saved in the project. Run status, progress,
   ETA and logs are runtime-only. The schema moves from 1.28 to 1.29 with a version-keyed migration
   that gives older projects an empty queue.
2. Exports render in the background while the artist keeps editing. The Viewer has strict GPU
   priority: during Viewer activity, export uses only spare capacity, with no guaranteed rate. Full
   speed returns when the Viewer is idle. Unproven GPU retirements are recovered.
3. The Export panel replaces the export dialogs. Export menu items add the composition to the
   queue and reveal the panel. Video comes first: a new job's default output is MP4/H.264, and image
   sequences are first-class presets but never the default. There is no promoted still-frame
   action; a single frame is an ordinary range choice.
4. Linux is the delivery target. On macOS and Windows the queue is fully editable and Start is
   unavailable with a typed reason; export there is tracked by #371 (macOS) and #370 (Windows).
5. An existing destination is replaced only after the artist confirms it.
6. HEVC is hardware-only (VA-API).
7. An output may import its result into the project's Assets after a verified publish.
8. Everything ships in one release, including issue #369 (export verified end to end on every
   route, color tags, MP4/H.264, HEVC, DPX, threading and size limits).

The design must keep Qt in `src/ui` and `apps`, keep module dependencies acyclic, reuse the proven
attempt/approval/publication pipeline (frame-output.md) instead of replacing it, and keep GPU
coverage unchanged: the queue adds no pixel operation.

## Decision

### 1. Ownership and modules

| Module | Owns |
| --- | --- |
| `src/document` | durable records: `ExportQueue`, `ExportJob`, `ExportOutput`, `ExportOutputSettings`, ids, validation (`bloom/document/export_queue.hpp`) |
| `src/commands` | the undoable queue edits (`export_queue_operations.hpp`); every one has `renderAffecting() == false` |
| `src/project` | schema 1.29, the queue codec and the 1.28 → 1.29 migration |
| `src/output` | the #369 formats (MP4, HEVC, DPX), color-tag derivation, encoder threading, capacity-derived limits, reopen and quality-check primitives |
| `src/runtime` | `GpuAdmissionArbiter` over the shared `GpuProcessingPhasePolicy` (section 9) |
| `src/render` | the export device's low queue priority where supported and the batch-yield hook |
| `src/platform` | free-space queries, staged streaming hand-off to the worker, destination-scoped stale-staging cleanup |
| **`src/host`** | **the queue's runtime owner**: `ExportQueueService`, `ExportJobRunner`, `ExportPresetCatalogue` and the user-preset store, preflight sampling, availability, snapshots, events, logs, the subscription, verification and import results, `resolveDestination()`, `exportJobScript()` (`bloom/host/export_queue.hpp`, `export_presets.hpp`, `export_verification.hpp`) |
| `src/scripting` | `bloom.export.*` operations and the session calls (section 12) |
| `src/ui` | the Export editor panel, the application-lifetime queue adapter, menus, the Jobs row link, the replace-confirmation and OpenH264 consent/locate dialogs |
| `apps/bloom`, `apps/bloom-cli`, `apps/bloom-mcp` | wiring, shutdown registration, `bloom-cli export`, MCP tools |

`src/host` already owns `ProjectSession`, `PublicationCoordinator`, `GpuExportProvider`,
`SequenceExportRunnerV1` and the attempt runner, and every edge the queue needs (`host` → document,
commands, output, runtime, platform, project, color, core) is already in the repository's allowed
module dependencies. No new module edge is added and the graph stays acyclic. A separate
`src/export` module was rejected: it would need `ProjectSession` and sit above `host` with no
independent owner. The queue adds no node parameter, so the parameter-presentation registry gains
no entry.

### 2. Project truth and runtime state

**Project truth** (saved, undoable, dirties the project): the ordered jobs; per job the composition,
an optional name, the render settings (the frame range), the enabled flag and an optional label
color; per output the origin preset id, the full `ExportOutputSettings`, the destination with its
overwrite policy, the enabled flag and the import-into-Assets flag. An output keeps every setting it
needs, so a project exports the same way on a computer that lacks the user preset it was made from.

**Runtime state** (never saved, owned by `ExportQueueService`): job and output states, progress,
ETA, the per-run log, failures, cached destination samples, replace confirmations, verification
results, import results, the device route and the link to the task system. Opening a project shows
every job **Ready**.

**Preferences** (per user, outside projects, not undoable): user presets, stored as a versioned
JSON file (`bloom.export-presets`, version 1) in the platform user-data directory and read and
written on the blocking-I/O executor.

The service reads project truth only from the session's published immutable snapshots. Queue edits
are ordinary commands, so they share validation, dirty state, undo and the event path with every
other edit. They are not render-affecting and invalidate no preview cache. The one document change
the service itself issues is the asset import an output explicitly asks for (section 8).

### 3. Schema 1.29

`project` gains a required last member `exportQueue`: `{ "jobs": [ ... ] }` in run order.
`idAllocation.highestIssued` appends `exportJob` and `exportOutput` after `layerMask`.

| Object | Members in writer order |
| --- | --- |
| job | `id`, `composition`, `name`, `range`, `outputs`, `enabled`, optional `labelColor` |
| range | `kind` (`"work-area"`, `"composition"`, `"custom"`, `"frame"`), `first`, `last` (decimal strings; `last` only for `custom`) |
| output | `id`, `preset`, `settings`, `destination`, `enabled`, `importIntoAssets` |
| settings | `format`, `codecProfile`, optional `videoBitrateKbps`, `compression`, `outputColorSpace`, `display`, `view`, `bakeLook`, `includeAudio`, `audioSampleRate`, `encoder` (`"automatic"`, `"software"`, `"hardware"`), `naming` |
| naming | optional `startFrame`, `framePadding`, `pattern` |
| destination | `portability` (`"project-relative"`, `"external"`), `path`, `overwrite` (`"ask"`, `"replace"`, `"refuse"`) |

`labelColor` uses the layer label representation: three integers `0..255`. `format` tokens are
the closed 1.29 vocabulary: `png`, `exr`, `tiff`, `dpx`, `prores-mov`, `dnxhr-mxf`, `h264-mov`,
`h264-mp4`, `hevc-mov`, `hevc-mp4`, `wav`. Ids are allocator-backed decimal strings and
project-global per namespace. Bounds: 1,024 jobs, 16 outputs per job, 4 KiB per string. A job's
composition must exist: deleting a composition removes its jobs in the same transaction, and undo
restores both. Destination paths follow the 1.22 locator rules (relative to the project folder when
representable, otherwise an RFC 8089 file URI) and may contain the tokens of section 7.

Migration `migrateExportQueueV1_28` (1.28 → 1.29) is a self-contained, version-keyed DOM step. It
appends `"exportQueue": {"jobs": []}` and the two zero high-water members, and its
`alreadyMigrated` guard checks for their presence. Tests: a 1.28 fixture opens with an empty queue
and equals the same project saved at 1.29; a full queue round-trips byte for byte; the chained
migration count gains one step; newer-minor pins move to 1.30; `document-1.29` and `manifest-1.29`
schemas and their checkers are added.

### 4. Job lifecycle

```text
Ready --Start--> Waiting --turn--> Preparing --> Rendering --> Verifying --> Done | DoneWithWarnings
Rendering --Pause--> Pausing --frame boundary--> Paused --Resume--> Rendering
Waiting --Stop--> Ready
Preparing | Rendering | Pausing | Paused | Verifying --Stop--> Stopping --> Stopped
Preparing | Rendering | Verifying --error--> Failed
Done | DoneWithWarnings | Failed | Stopped --Retry--> Waiting
```

- **Preparing** captures the session's newest published document snapshot, resolves the range and
  freezes the `{date}` token. Edits made after this point never change a running job. Preparing then
  compiles the plan, resolves each enabled output's settings against the catalogue and the project
  color settings, revalidates preflight on a worker (authoritative over the cached sample), and
  starts the first frame's analysis.
- **Rendering** runs the outputs one after another. Each output has the states Pending → Preparing
  → Rendering → Verifying → Published | PublishedWithWarnings | Failed | Stopped. A disabled
  output, or an output after a failed one, is Skipped.
- **Verifying** runs the job-level checks (section 10) over what was published.
- A job becomes terminal only after every requested asset import has resolved (section 8).
- Edits to a running job (Preparing through Stopping) are unavailable with the reason "Stop the job
  to change it; it keeps the settings it started with". If the record changes anyway (undo, a
  script), the run continues with what it captured and the view sets `recordChangedSinceStart`. If
  the record disappears, the run stops with `JobRemoved`. Waiting and terminal jobs are editable.

### 5. Presets and defaults

The catalogue (`ExportPresetCatalogue`) lists built-in presets, then user presets, in category
order **Video**, **VFX / sequences**, **Audio**, **User**:

- **Video**: H.264 MP4 (`builtin:h264-mp4`, the default), H.264 MOV, Review H.264 (Rec.709, look
  on), HEVC MP4/MOV (hardware), ProRes MOV (Proxy through 4444 XQ), DNxHR MXF.
- **VFX / sequences**: VFX handoff EXR (ACES2065-1, PIZ, 1001, ####), OpenEXR, DPX, PNG and TIFF
  sequences. An image preset writes a single still when the job's range is one frame.
- **Audio**: PCM WAV/BWF.

Each preset carries its settings, its capabilities (sequence, single file, still, alpha, audio,
extension), its **expected preservation** (the eleven facet states the format is declared to
produce) and a **platform availability** with a typed code, a tooltip-ready reason and a remedy
(Install OpenH264, Locate OpenH264, Open Project Settings, Choose Software Encoder).
Project-dependent requirements (the named deliverables need the built-in ACES CG config with a
pinned revision; output color names must exist in the config) are evaluated per output by
`availabilityFor()`. This replaces `composition_export_capability.hpp`.

Encoder availability comes from a worker probe published as a neutral `ExportEncoderStatus`. In the
desktop application the existing `H264EncoderService` keeps its probe, consent and locate flow and
forwards its state with `setEncoderStatus()`; headless owners call `refreshEncoders()`. HEVC has no
software encoder: without a VA-API render node that exposes HEVC encode, HEVC presets report
`HardwareEncoderMissing`.

**Defaults.** **Add to Export Queue** creates a job with the work-area range and one output from
`defaultPreset()`: H.264 MP4 when it is available here, otherwise the stated fallback video preset,
ProRes 422 HQ MOV, with the H.264 reason recorded (`ExportDefaultPreset::preferredAvailability`).
If neither is available (macOS and Windows in this version), the output is still H.264 MP4 and its
availability reason blocks Start. The default is never an image sequence and is not affected by
the last preset used. The default destination is `Exports/{name}.<ext>` beside the project, with
overwrite policy AskBeforeReplacing and import into Assets off. An unsaved project has no default
destination, and Start says "Choose where to save this output".

### 6. The runner

- **One job at a time, in queue order; outputs of a job in order.** Concurrency is not
  configurable. The export evaluator owns one GPU device generation that admits one processing
  phase, so two concurrent jobs would split the same device and double memory without more total
  throughput. Parallelism lives inside a job: row-band CPU evaluation, multithreaded encoders
  (#369), and evaluation of frame N+1 overlapping the encode of frame N, as
  `SequenceExportRunnerV1` already does (one acknowledged product in flight).
- **Each output is its own pass** through the existing path: `beginOutputAnalysisAttemptV1` →
  policy approval (section 10) → `executeExportPublication` per frame for images, and
  `SequenceExportRunnerV1` for movies and WAV. A shared evaluation fanned out to several outputs
  was rejected: each frame's analysis digest binds one preset and one display processor, so
  fan-out would need a new multi-sink attempt graph. The operation cache makes the repeated
  evaluation of a second output cheaper.
- **Threading and notification.** All work is scheduler tasks at `Foreground` priority, owned by
  `TaskOwner{Export, owner}` and grouped per run. Section 11 states the caller-thread guarantees.
- **Cancellation (Stop).** Stop cancels the run's task group. A movie or WAV removes its staged
  file and leaves the destination unchanged. A sequence keeps the frames it already published,
  because each frame is its own atomic publication, and removes the staged frame. A Waiting job
  simply returns to Ready.
- **Pause** acts at a frame boundary. In-flight native GPU work cannot be pre-empted, so Pausing
  lasts at most one frame. A paused sequence holds nothing. A paused movie keeps its encoder
  session, staged file and queue byte reservation, but no GPU phase. Paused time and time spent
  WaitingForViewer are excluded from the export's no-progress and total deadlines: the runner
  passes an `OutputExportClockV1` that stops in both states, and the encode session's per-request
  timeout applies only to requests in flight. Preparing and Verifying are short and cannot be
  paused.
- **Safe shutdown.** Quitting, closing the project or opening another one with
  `activeJobCount() > 0` asks: "N exports are running. Stop them and quit?" (**Stop and Quit** /
  **Keep Exporting**, with Keep Exporting the default). Confirming calls `beginShutdown(cause)` and
  keeps pumping while the UI stays responsive until `quiescent()`. The existing
  `ApplicationShutdownCoordinator` then retires the GPU provider and the
  `SequenceExportRetirementOwnerV1`. Saving the project during an export is allowed.
- **Crash and partial files.** No run state is persisted, so a crash cannot leave a stale
  "running" job. A movie's destination is untouched, because only a verified staged file is
  published. A sequence holds only complete, verified frames. When an output next runs,
  `src/platform` removes stale `.bloom-stage-` files for that exact target in that destination
  folder whose recorded owner process is not alive. This cleanup is destination-scoped and never
  touches other files. Worker crashes, disk full and deadline expiry end the output with a typed
  failure, and Retry starts a fresh run.

### 7. Destinations, tokens, preflight and replacement

- **Tokens.** A destination path and a sequence's naming pattern accept `{name}` (composition
  name), `{job}` (the job's display name), `{format}` (the format's short name, such as `mp4`,
  `exr` or `prores`), `{date}` (YYYY-MM-DD, frozen when the run starts), `<base>` and `<ext>` (the
  destination's stem and extension), and, for sequences, exactly one `####` or `{frame}` padded to
  the naming's frame padding and widened when the range needs more. Values are sanitized for file
  names. An unknown token, a separator produced by a value, or a changed extension is a
  `NamingInvalid` issue. `host::resolveDestination(job, output, context)` is pure (the context
  supplies the project folder, composition name, range and date), so the panel previews the first
  and last file live. The run uses the same function.
- **Preflight.** Each `ExportPreflightIssue` has a code, a **target** (Job, Composition, Range,
  Destination, Naming, Container, Codec, Color, Audio, Encoder) so the panel can mark the field,
  a **severity** (Blocking, Warning, NeedsConfirmation) and a tooltip-ready reason. Pure checks
  (naming, ranges, duplicate targets, preset availability) run in `pump()`. Destination facts
  (exists, folder writable, free space, the existing file's fingerprint or, for a sequence, the set
  of existing files the pattern would write) are sampled on the blocking-I/O executor and cached.
  A sample is taken when an output's destination or settings change, when a job enters Waiting,
  and on `refreshPreflight()`. Until the first sample lands, Start is unavailable with
  `DestinationCheckPending` ("Checking destination…").
- **Size.** Each output view has an optional `estimatedBytes`, computed from the format, profile
  or bit rate, extent and frame count. It is absent when the format's size cannot be estimated
  honestly, such as compressed EXR. Each view also has an optional `freeBytesAtDestination` from
  the worker sample. When both are present and the estimate plus a safety margin exceeds the free
  space, preflight reports a Blocking `InsufficientSpace`. During the write the runner rechecks
  free space.
- **Replacement.** The overwrite policy is **AskBeforeReplacing** (the default), **Replace** or
  **Refuse**. With an existing destination, preflight reports `DestinationExists` as
  NeedsConfirmation, Warning or Blocking respectively.
  - Under AskBeforeReplacing, Start is unavailable (`NeedsReplaceConfirmation`) until the artist
    calls the session command `confirmReplace(job, output)`. The confirmation binds the sampled
    fingerprint, or for a sequence the sampled file set.
  - The confirmation is cleared when the destination, the output's settings or the sampled files
    change, and the run that replaces the files consumes it.
  - A file this session itself published for that output needs no confirmation on a re-run.
  - The external-modification guard still applies at publication. A file changed after the sample
    or the confirmation fails with `PublicationConflict` and is never replaced.

### 8. Import into Assets after render

`importIntoAssets` is an optional per-output flag. It is project truth, undoable and off by
default.

- **When it runs.** After that output's verified publish, before the job's next output starts,
  the runner prepares the import on a worker through the existing asset import path. A movie
  becomes a Video asset, a sequence a Sequence asset, a still an Image asset and a WAV an Audio
  asset. `pump()` then applies one ordinary undoable import command on the owner thread. If an
  interactive edit gesture is open, the command waits until the gesture closes.
- **Re-runs.** If an asset already refers to the same file, it is re-identified through the
  ADR 0025 source-identity path instead of duplicated (`Refreshed`).
- **Failure handling.** The export has already succeeded and stays succeeded. Its state and
  verification are unchanged. An import failure (read-only project, probe failure, project closed)
  is reported only in that output's `ExportAssetImportResult`, with a stable code and a reason. The
  result appears in the snapshot and in every terminal event's `imports`, and
  `ExportQueueDrained::importFailures` counts these failures. An output that did not publish
  reports `Skipped`.
- **Headless.** `bloom-cli export` applies imports to its session and writes the project only with
  `--save`. Without it, the import results say the change was not saved.

### 9. GPU admission with strict Viewer priority

Today preview and export each own a GPU device generation, and both draw on one shared processing
root (`GpuProcessingPhasePolicy`). Each generation retains at most one processing phase, and an
unproven retirement latches it. A refused request returns an `admissionRetry` watermark and its
caller retries when that watermark moves. Under a long export that model lets export hold the root's
bytes while the Viewer is refused, and an unproven export retirement latches the export generation
for every subsequent frame.

`runtime::GpuAdmissionArbiter`, constructed once by the composition root around the shared policy
and passed explicitly to the preview service and the export provider (no locator), replaces
refuse-and-retry for phase reservation:

1. **Classes.** *Viewer* (the current Viewer frame, scrub, playback and RAM preview), *Export*
   (final render: still, sequence, movie and headless, GPU or CPU-reference host phases), and
   *Background* (thumbnails and speculative work).
2. **Viewer activity.** The Viewer is active while a Viewer request waits or holds a phase, or
   while playback or RAM preview runs. The preview owners report playback and RAM preview through
   an explicit activity flag. The Viewer is idle otherwise.
3. **Strict priority.** While a Viewer request waits, no Export or Background phase is granted. A
   Viewer request never queues behind an export; the preview's one-active/one-newest gate is
   unchanged.
4. **Spare capacity only.** While the Viewer is active, an Export grant is made only when no Viewer
   request waits and the grant leaves room on the host and the device for the Viewer reserve. The
   reserve is the last admitted Viewer envelope, or the active composition's Viewer envelope before
   any Viewer frame has been admitted. There is no minimum export rate: continuous playback may
   hold export in `WaitingForViewer` indefinitely, and the panel shows that state with no ETA. When
   the Viewer is idle, Export is granted whenever the root admits it (full speed). Background ranks
   below Export.
5. **Frame-sized grants.** An export phase covers exactly one frame, so export yields at every frame
   boundary. If the Viewer's envelope grows while an export phase is held (for example, the Viewer
   resolution changes) and the reserve no longer fits, the Viewer waits for that one export frame,
   and no further export frame is granted.
6. **Waiting, not refusal.** An Export request waits in FIFO order with the typed status
   `WaitingForViewer` until a lease release revision, a fresh sample epoch, a quarantine retirement
   or the Viewer going idle admits it. These are signals that exist today plus the activity flag, so
   there is no polling loop. Explicit caller caps still refuse.
7. **Sharing device time.** The export device requests LOW global queue priority through
   `VK_KHR_global_priority` / `VK_EXT_global_priority` when the Linux driver exposes it. Export
   native work is already split into bounded command batches. While the Viewer is active, the export
   owner thread submits its next batch only when no Viewer submission is pending. Where global
   priority is absent, batch yielding alone applies.
8. **Recovering unproven retirements.** When an export frame's phase retirement cannot be proven,
   the provider marks that generation quarantined in the arbiter. Its charges stay charged until its
   own fence or teardown proof; a timeout never releases them. The provider starts the generation's
   asynchronous retirement and bootstraps a fresh generation (a new ownership epoch) on a worker,
   with the job's route shown as `RecoveringGpu`. The failed frame is then retried on the new
   generation. At most one export generation can be quarantined at a time. If a second unproven
   retirement occurs, or the replacement fails to bootstrap within 30 s, the remaining frames of
   that output use the explicit CPU reference route when host admission allows it (the
   `CpuReference` route, a DeviceProvenance warning, never a frame labelled GPU). Otherwise the
   output fails with `ResourceRefused`. A quarantined Viewer generation stays owned by the preview
   service; the arbiter counts its held charges toward the Viewer reserve, so exports wait rather
   than take the Viewer's room.

The one-phase-per-generation rule in `GpuDevice` is unchanged. The arbiter governs the root that
the two generations share; inside the export generation, one phase means one frame at a time, which
is what the serial runner does. A `--require-device` export still fails instead of falling back.
This strict priority replaces, for GPU phase admission, task-system.md's aging rule that scrubbing
must not starve an explicit export. The CPU and I/O scheduler keeps that rule.

### 10. Verification replaces the approval prompt

- **Before Start**, every output shows its preset's expected preservation, its preflight issues,
  its resolved first and last file and its size facts (section 7).
- **Approval by policy.** The queue approves each frame's analysis digest under the headless
  policy that frame-output.md already allows: the approval is accepted when every facet state fits
  the preset's declared expected preservation. A frame that does not fit fails the output before
  publication (`AnalysisRefused`, check `AnalysisPolicy`). The byte-equality guard and per-frame
  publication intent are unchanged; only the modal question goes away.
- **After publication**, each output carries an `ExportOutputVerification`. It contains the
  accepted facet states and digest, an artifact summary (count, first and last path, bytes, digest,
  worst publication outcome), device provenance, and typed `ExportCheckResult`s for Reopen,
  FrameCount, Timing, ColourTags, Container, Audio, Naming, ArtifactDigest, Publication and
  DeviceProvenance, each with expected and observed values. Overall: Failed if any check failed,
  Warning if any warned, else Passed. Any frame evaluated on the CPU reference path is a
  DeviceProvenance warning, so such a frame is never presented as GPU.

### 11. What the panel reads, and from which thread

- **Non-blocking guarantee.** Every call the panel makes is non-blocking and performs no I/O on the
  calling thread: `snapshot()`, `events()`, `log()`, `availability(...)`, `canStart()`, every
  session command, `pump()`, `refreshPreflight()`, and the catalogue's `snapshot()`,
  `defaultPreset()` and `availabilityFor()`. Filesystem facts come only from cached worker samples
  (section 7).
- **One publisher.** Only `pump()` publishes a new `ExportQueueSnapshot`, so the revision the UI
  thread sees advances in one place and never shows half-applied state. A session command records
  its intent and runs `pump()` before returning; its result carries that pump's revision.
  `availability(...)` answers from the last published snapshot, so it never disagrees with it.
- **The drain.** In the desktop application one application-lifetime Qt adapter in `src/ui`, owned
  by the composition root like today's export controller, calls `pump()` from
  `TaskUiBridge::resultsAvailable`. That is the coalesced completion drain that the preview, asset,
  cache-purge and H.264 consumers already use. When the revision changed, the adapter emits one
  "queue changed" signal that the Export panels and the Jobs row read. Panels are replaceable and
  may be closed or duplicated, so no panel owns the pump. Every export task, destination sample
  and import preparation is a scheduler task, so its completion wakes that drain.
- **Worker threads.** `subscribe()` returns an `ExportQueueSubscription` with the neutral
  serial-wake shape of `runtime::TaskCompletionSubscription` (eight slots, stop-aware wait,
  idempotent close). It is for scripting and MCP waiters. The UI thread never calls
  `waitForChange()`.
- **Snapshot.** `ExportQueueSnapshot` has a monotonic `revision` and per-job `revision`s for cheap
  diffing, job and output views, and the availability of every command at every level. A job view
  carries only its log size, newest sequence and newest entry; the panel reads the log itself with
  `log(job, afterSequence)` (bounded, 1,024 entries per run, with a `gap` flag).
- **Terminal events.** `events(afterSequence)` returns typed terminal events from a bounded ring
  (256 events). Each event has a sequence and the revision whose pump published it:
  - `ExportJobSucceeded`, with or without warnings;
  - `ExportJobFailed`, with a structured `ExportFailure`;
  - `ExportJobCancelled`, with the stop cause;
  - `ExportQueueDrained`, with counts since the runner left Idle, including import failures.

  Each job event carries its outputs' verification and, separately, their import results. A
  `gap` flag reports overflow; the snapshot still holds every result. The engine has no notion of
  sounds or notifications; a UI service may react to these events.
- **Jobs link.** Every task a run submits uses `TaskOwner{TaskOwnerKind::Export, owner}` and one
  `TaskGroupId` per run (`ExportRunLink`). The Jobs panel collapses an Export-owned group into one
  row. That row reads its progress from the queue snapshot through `jobForTaskOwner()` and offers
  **Show in Export**. Export-owned task rows offer no cancel, pause or retry; the Export panel owns
  those. There is one progress source, the queue snapshot.
- **Copy as command.** `host::exportJobScript(job, CliJson | Python)` is pure. It emits a
  `bloom.export.*` transaction that recreates the job and all its outputs, followed by an
  `export.start` call.
- **Editor type.** The panel is a registered editor type, "Export". The UI side reveals it.

### 12. Commands and availability

Every command has an availability with a typed `ExportUnavailableCode` and a tooltip-ready reason,
so no control is disabled silently. Project-truth commands are `src/commands` operations submitted
on the ordinary command path (the panel through the session, scripts in transactions). Session
commands are `ExportQueueService` methods. On macOS and Windows, Start, Start All and Retry answer
`PlatformNotSupported` ("Export runs on Linux in this version"); every other command behaves as on
Linux.

| Command | Kind | Available when (otherwise the stated reason) | Script / MCP |
| --- | --- | --- | --- |
| Add to Export Queue (`AddJob`) | Project truth (undoable) | a project is open and editable, the composition exists, fewer than 1,024 jobs | `bloom.export.add-job` |
| Remove job (`RemoveJob`) | Project truth | the job is not running (Ready, Waiting or terminal) | `bloom.export.remove-jobs` |
| Remove finished (`RemoveFinished`) | Project truth | at least one job is Done or DoneWithWarnings | `bloom.export.remove-finished-jobs` |
| Duplicate job (`DuplicateJob`) | Project truth | the job exists and the queue has room; the copy has new ids and is Ready | `bloom.export.duplicate-job` |
| Move job (`MoveJob`) | Project truth | the job is not running; moving a Waiting job changes run order | `bloom.export.move-jobs` |
| Edit job: name, range, enabled, label color (`EditJob`) | Project truth | the job is not running | `bloom.export.set-job-name`, `set-job-range`, `set-job-enabled`, `set-job-label-color` |
| Add output (`AddOutput`) | Project truth | the job is not running and has fewer than 16 outputs | `bloom.export.add-output` |
| Edit output: settings, destination and overwrite policy, enabled, import into Assets (`EditOutput`) | Project truth | the job is not running | `bloom.export.set-output-settings`, `set-output-destination`, `set-output-enabled`, `set-output-import` |
| Apply preset (`ApplyPreset`) | Project truth | the job is not running and the preset exists (an unavailable preset may be applied; its reason then blocks Start) | `bloom.export.apply-preset` |
| Remove output (`RemoveOutput`) | Project truth | the job is not running and has more than one output | `bloom.export.remove-output` |
| Start (`Start`) | Session | Linux; not shutting down; the job is not active; the composition exists and the range resolves; at least one enabled output; every enabled output has a destination sample, no Blocking issue, no unavailable preset, and any NeedsConfirmation issue confirmed | `export.start` |
| Start all (`StartAll`) | Session | at least one enabled job that Start would accept; jobs Start would refuse stay Ready with their reasons | `export.start-all` |
| Pause (`Pause`) | Session | the job is Rendering | `export.pause` |
| Resume (`Resume`) | Session | the job is Paused | `export.resume` |
| Stop (`Stop`) | Session | the job is active (Waiting returns to Ready; the others go to Stopping) | `export.stop` |
| Stop all (`StopAll`) | Session | at least one job is active | `export.stop-all` |
| Retry (`Retry`) | Session | the job is terminal and Start's other conditions hold | `export.retry` |
| Confirm replace (`ConfirmReplace`) | Session | the output's policy is AskBeforeReplacing, its sampled destination exists and is not yet confirmed, and the job is not running | `export.confirm-replace` |
| Reveal output (`RevealOutput`) | Session | the output published at least one artifact in this session | (UI only) |

Menus: **File → Add to Export Queue** (Ctrl+M) (also on Assets composition rows) adds a job for the
composition and reveals the panel. The former Export Frame, Export Frame Range and Export
Composition items are replaced by it. A single frame or a range is chosen in the job's range, with
any preset.

### 13. Scripting, MCP and the CLI

- **Transactions** gain the `bloom.export.*` project-truth operations in the table. They have the
  same validation and undo as panel edits, and `add-job` returns the new `ExportJobId` and
  `ExportOutputId`s.
- **Session calls**, which are not transactional: `export.start`, `export.start-all`,
  `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`,
  `export.confirm-replace`, `export.status` (the snapshot as JSON, optionally `sinceRevision`),
  `export.events` (`afterSequence`), `export.log` (`job`, `afterSequence`), `export.wait` (a job or
  drain, with a timeout; headless only), `export.presets` and `export.script` (`exportJobScript`).
  MCP exposes the same set as tools, under the existing request limits. An unattended caller sets
  the overwrite policy to Replace or confirms explicitly; nothing replaces a file silently.
- **One runner.** The existing `render` tool, `scripting::Render::run` and `bloom-cli render` keep
  their request shapes. They run a transient, unsaved job through the same `ExportJobRunner`, so
  every route uses the same pipeline and verification.
- **`bloom-cli export --project <file> (--all | --job <id>...) [--replace] [--save]`** renders the
  saved queue headlessly. It exits non-zero unless every selected job succeeds, and it prints the
  verification and import results as JSON. `--replace` confirms every NeedsConfirmation issue.
  `--save` writes asset imports back to the project. This is the headless route of the GPU
  coverage contract.

### 14. Reuse and what leaves `src/ui`

- **Moves to `src/host`** as `ExportJobRunner`:
  - the still and range driver of `FrameExportController`: plan compile, per-frame attempts, the
    single approval for a range, sequence naming, `SequenceState`;
  - the composition-export driver (`frame_export_controller_composition.cpp`);
  - the COLOR-5 deliverable color preparation (`frame_export_controller_deliverables.cpp`);
  - the facet and failure descriptions, which become typed results;
  - `composition_export_capability.hpp`, which becomes `ExportPresetCatalogue::availabilityFor()`.
- **Reused unchanged**: `OutputAnalysisAttemptRunnerV1`, `approveFrameExportV1`,
  `executeExportPublication`, `SequenceExportRunnerV1`, `SequenceExportRetirementOwnerV1`,
  `GpuExportProvider`, `PublicationCoordinator`, `StagedArtifactCoordinator`,
  `ExportResourceLedgerV1`, `FrameRangeRunnerV1`, the reopen verifiers and the asset import
  preparation.
- **Removed from `src/ui`**: `FrameExportController` and `frame_export_controller*.cpp`, and
  `composition_export_dialog.*`, whose fields become the panel's output editor. The approval and
  range dialog builders and their review captures are removed with them.
- **Stays in `src/ui`**: `H264EncoderService` (it forwards its state to the catalogue, and its
  consent and locate flow is the remedy for `EncoderNotInstalled`); the new Export editor; the queue
  adapter; the Jobs row link; the shutdown prompt; the replace confirmation.

### 15. Platforms

Linux is the delivery target. The model, persistence, availability, panel, scripting and preset
editing are identical on Linux, macOS and Windows. Paths use the native `std::filesystem::path` with
the 1.22 locator rules, user presets live in the platform user-data directory, and a 1.29 project
saved on any platform opens on every other.

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Queue, panel, persistence, presets, scripting edits | yes | yes | yes |
| Start, Start All, Retry, `bloom-cli export`, `render` | yes | `PlatformNotSupported` (#371) | `PlatformNotSupported` (#370) |
| H.264 | OpenH264 after consent, or VA-API | — | — |
| HEVC | VA-API only; otherwise `HardwareEncoderMissing` | — | — |
| GPU low priority for export | global priority where the driver exposes it; batch yield otherwise | — | — |

On macOS and Windows every preset's platform availability is `PlatformNotSupported` with the reason
"Export runs on Linux in this version". This is the explicit supported fallback AGENTS.md requires:
nothing is hidden, nothing fails silently, and a project prepared there exports unchanged on Linux.
The Windows publication provider and encode worker, and macOS export qualification, are not part of
this release; #370 and #371 track them.

### 16. Issue #369 inside this release (Linux)

- **End-to-end routes.** Still, sequence, video with audio, and headless/CLI are verified on a
  reference project with layers, masks, retimed video and an untagged H.264 source. Every route
  shows progress, can be stopped, and ends with a typed result naming the output location. The
  queue's verification is that result.
- **Color tags** come from the output's color intent (display/view, or the output color space)
  instead of fixed BT.709. They are checked by `ColourTags` on reopen.
- **MP4 container** for H.264 and HEVC (`h264-mp4`, `hevc-mp4`), alongside MOV.
- **HEVC** through VA-API only. There is no software HEVC encoder.
- **DPX sequences** (10-bit and 16-bit RGB) through the worker with a reopen verifier, carrying the
  output color space like EXR.
- **Multithreaded encode.** The worker uses an automatic thread count. Each preset's
  `MediaDeterminismV1` states whether threading can change the bitstream. When it can, the tolerance
  profile covers decoded frames, not bytes.
- **Limits from destination capacity.** The fixed 16 GiB output cap and 8-channel audio cap are
  replaced by the destination's free space minus a safety margin, and by each container's own
  limits (section 7).
- **Staged streaming publication.** The worker writes directly to the coordinator's staged file
  through an inherited descriptor, instead of returning chunks into a capped temporary copy.
- **VA-API render-node discovery** (render node plus H.264 and HEVC encode entry points) is part of
  the encoder probe.

### 17. Review requests declined or adjusted

- **The pump is not owned by the panel.** The review asked which drain the panel hooks. The pump
  is driven by one application-lifetime adapter on `TaskUiBridge::resultsAvailable`, not by a
  panel, because Export panels are replaceable, may be closed, and may exist more than once. Panels
  connect to the adapter's "queue changed" signal.
- **No output-name token.** Outputs have no name in the model. `{format}` and `{job}` cover the
  stated use, and adding a name field only for a token was rejected.
- **The job label is not mirrored from the composition.** Bloom has label colors on layers but
  not on compositions, so there is nothing to mirror. `labelColor` is the job's own optional label
  and uses the layer representation.
- **The free-space and estimate fields can be absent**, as the review allowed. An estimate is
  omitted rather than guessed for formats whose size depends on content.

## Consequences

- Export settings survive save and reopen, and many deliverables can be prepared and run while the
  artist keeps working. The modal approval prompt disappears; its information moves to the preset's
  expected preservation before Start and to the verification afterwards.
- One runner serves the panel, scripting, MCP and the CLI. `src/ui` loses about 2,500 lines of
  export orchestration.
- Document 1.29 adds project members and allocator namespaces. Older projects open with an empty
  queue. Same-major newer minors keep their additive members as today.
- During playback or RAM preview an export can wait without progress. The panel says so
  (`WaitingForViewer`, no ETA), and deadlines do not count that time.
- macOS loses the PNG and OpenEXR export the dialog offered there; export on macOS returns with
  #371.
- The arbiter changes preview admission from refuse-and-retry to queued grants with strict Viewer
  priority. The preview route proofs and the GPU coverage native acceptance must pass unchanged,
  with export running concurrently in an added witness.
- The GPU coverage contract is unchanged: the queue adds no operation, kernel or lowering, and
  creates no opt-out.

## Alternatives rejected

- **Keeping the queue in `src/ui`** behind `FrameExportController`: Qt would own export
  orchestration, and headless routes would keep separate code.
- **Configurable concurrent jobs**: the export device admits one phase, so concurrency adds memory
  pressure and Viewer contention without total throughput.
- **One evaluation fanned out to several outputs**: see section 6.
- **Pausing in-flight GPU work**: native submissions cannot be pre-empted; the frame boundary is
  the honest pause point.
- **Keeping refuse-and-retry admission**: it lets a long export refuse Viewer frames.
- **A guaranteed minimum export rate during playback**: rejected by the owner in favor of strict
  Viewer priority.
- **Device priority alone**: it is not available on every driver.
- **Silent replacement or hard refusal as the default**: the owner chose an explicit confirmation.
- **Failing the export when its asset import fails**: the file was verified and published; the
  import is reported separately.
- **Callbacks for progress**: they break the neutral-subscription rule.
- **Saving run status or verification**: excluded by the owner, and stale after any edit.
- **Defaulting new jobs to the last-used preset**: it could make an image sequence the default,
  which the owner excluded.

## Owning documents the implementation updates

`architecture/frame-output.md` (queue, policy approval, verification, replacement confirmation),
`architecture/task-system.md` (the Export owner kind and Jobs link, shutdown with exports, strict
Viewer priority for GPU admission), `architecture/project-format.md` (1.29),
`architecture/gpu-backend.md` (the arbiter), `architecture/media-io.md` (#369),
`architecture/platform-support.md` (export on Linux only in this version),
`user-guide/exporting-video.md` (the Export panel), `user-guide/mcp.md` and
`user-guide/scripting.md` (operations and `bloom-cli export`).
