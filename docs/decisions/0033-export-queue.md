# ADR 0033: Export Queue And The Export Panel

Status: proposed (design step; the supervisor may renumber the ADR and the schema minor at
integration)

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
2. Exports render in the background while the artist keeps editing. The Viewer keeps GPU priority.
   Export GPU work shares the device through a fair admission queue that also recovers unproven
   retirements.
3. The Export panel replaces the export dialogs. Export menu items add the composition to the
   queue and reveal the panel. Video comes first: a new job's default output is MP4/H.264, and image
   sequences are first-class presets but never the default. There is no promoted still-frame
   action; a single frame is an ordinary range choice.
4. Everything ships in one release, including issue #369 (export verified end to end on every
   route, colour tags, MP4/H.264, HEVC, DPX, threading and size limits).

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
| `src/output` | the #369 formats (MP4, HEVC, DPX), colour-tag derivation, encoder threading, capacity-derived limits, reopen/QC primitives |
| `src/runtime` | `GpuAdmissionArbiter` over the shared `GpuProcessingPhasePolicy` (section 8) |
| `src/render` | the export device's low queue priority where supported and the batch-yield hook |
| `src/platform` | free-space queries, staged streaming hand-off to the worker, destination-scoped stale-staging cleanup |
| **`src/host`** | **the queue's runtime owner**: `ExportQueueService`, `ExportJobRunner`, `ExportPresetCatalogue` and the user-preset store, availability, snapshots, events, subscription, verification aggregation (`bloom/host/export_queue.hpp`, `export_presets.hpp`, `export_verification.hpp`) |
| `src/scripting` | `bloom.export.*` operations and the session calls (section 11) |
| `src/ui` | the Export editor panel, the subscription adapter, menus, the Jobs row link, the OpenH264 consent/locate dialogs |
| `apps/bloom`, `apps/bloom-cli`, `apps/bloom-mcp` | wiring, shutdown registration, `bloom-cli export`, MCP tools |

`src/host` already owns `ProjectSession`, `PublicationCoordinator`, `GpuExportProvider`,
`SequenceExportRunnerV1` and the attempt runner, and every edge the queue needs (`host` → document,
commands, output, runtime, platform, project, color, core) is already in the repository's allowed
module dependencies. No new module edge is added and the graph stays acyclic. A separate
`src/export` module was rejected: it would need `ProjectSession` and sit above `host` with no
independent owner.

### 2. Project truth and runtime state

**Project truth** (saved, undoable, dirties the project): the ordered jobs; per job the composition,
an optional name, the render settings (the frame range) and the enabled flag; per output the origin
preset id, the full `ExportOutputSettings`, the destination and the enabled flag. An output keeps
every setting it needs, so a project exports the same way on a computer that lacks the user preset
it was made from.

**Runtime state** (never saved, owned by `ExportQueueService`): job and output states, progress,
ETA, the per-run log, failures, verification results, device route, and the link to the task
system. Opening a project shows every job **Ready**.

**Preferences** (per user, outside projects, not undoable): user presets, stored as a versioned
JSON file (`bloom.export-presets`, version 1) in the platform user-data directory and read and
written on the blocking-I/O executor.

The service reads project truth only from the session's published immutable snapshots and never
mutates the document. Queue edits are ordinary commands, so they share validation, dirty state,
undo and the event path with every other edit. They are not render-affecting and invalidate no
preview cache.

### 3. Schema 1.29

`project` gains a required last member `exportQueue`: `{ "jobs": [ ... ] }` in run order.
`idAllocation.highestIssued` appends `exportJob` and `exportOutput` after `layerMask`.

| Object | Members in writer order |
| --- | --- |
| job | `id`, `composition`, `name`, `range`, `outputs`, `enabled` |
| range | `kind` (`"work-area"`, `"composition"`, `"custom"`, `"frame"`), `first`, `last` (decimal strings; `last` only for `custom`) |
| output | `id`, `preset`, `settings`, `destination`, `enabled` |
| settings | `format`, `codecProfile`, optional `videoBitrateKbps`, `compression`, `outputColorSpace`, `display`, `view`, `bakeLook`, `includeAudio`, `audioSampleRate`, `encoder` (`"automatic"`, `"software"`, `"hardware"`), `naming` |
| naming | optional `startFrame`, `framePadding`, `pattern` |
| destination | `portability` (`"project-relative"`, `"external"`), `path`, `overwrite` (`"replace"`, `"refuse"`) |

`format` tokens are the closed 1.29 vocabulary: `png`, `exr`, `tiff`, `dpx`, `prores-mov`,
`dnxhr-mxf`, `h264-mov`, `h264-mp4`, `hevc-mov`, `hevc-mp4`, `wav`. Ids are allocator-backed
decimal strings and project-global per namespace. Bounds: 1,024 jobs, 16 outputs per job, 4 KiB per
string. A job's composition must exist: deleting a composition removes its jobs in the same
transaction, and undo restores both. Destination paths follow the 1.22 locator rules (relative to
the project folder when representable, otherwise an RFC 8089 file URI).

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

- **Preparing** captures the session's newest published document snapshot and resolves the range.
  Edits made after this point never change a running job. Preparing then compiles the plan,
  resolves each enabled output's settings against the catalogue and the project colour settings,
  runs preflight, and starts the first frame's analysis.
- **Rendering** runs the outputs one after another. Each output has the states Pending → Preparing
  → Rendering → Verifying → Published | PublishedWithWarnings | Failed | Stopped. A disabled
  output, or an output after a failed one, is Skipped.
- **Verifying** runs the job-level checks (section 7) over what was published.
- Edits to a running job (Preparing through Stopping) are unavailable with the reason "Stop the job
  to change it; it keeps the settings it started with". If the record changes anyway (undo, a
  script), the run continues with what it captured and the view sets `recordChangedSinceStart`. If
  the record disappears, the run stops with `JobRemoved`. Waiting and terminal jobs are editable.

### 5. Presets and defaults

The catalogue (`ExportPresetCatalogue`) lists built-in presets, then user presets, in category
order **Video**, **VFX / sequences**, **Audio**, **User**:

- **Video**: H.264 MP4 (`builtin:h264-mp4`, the default), H.264 MOV, Review H.264 (Rec.709, look
  on), HEVC MP4/MOV, ProRes MOV (Proxy through 4444 XQ), DNxHR MXF.
- **VFX / sequences**: VFX handoff EXR (ACES2065-1, PIZ, 1001, ####), OpenEXR, DPX, PNG and TIFF
  sequences. An image preset writes a single still when the job's range is one frame.
- **Audio**: PCM WAV/BWF.

Each preset carries its settings, its capabilities (sequence, single file, still, alpha, audio,
extension), its **expected preservation** (the eleven facet states the format is declared to
produce) and a **platform availability** with a typed code, a tooltip-ready reason and a remedy
(Install OpenH264, Locate OpenH264, Open Project Settings, Choose Software Encoder).
Project-dependent requirements (the named deliverables need the built-in ACES CG config with a
pinned revision; output colour names must exist in the config) are evaluated per output by
`availabilityFor()`. This replaces `composition_export_capability.hpp`.

Encoder availability comes from a worker probe published as a neutral `ExportEncoderStatus`. In the
desktop application the existing `H264EncoderService` keeps its probe, consent and locate flow and
forwards its state with `setEncoderStatus()`; headless owners call `refreshEncoders()`.

**Defaults.** **Add to Export Queue** creates a job with the work-area range and one output from
`defaultPreset()`: H.264 MP4 when it is available here, otherwise the stated fallback video preset,
ProRes 422 HQ MOV, with the H.264 reason recorded (`ExportDefaultPreset::preferredAvailability`).
If neither is available (today's Windows), the output is still H.264 MP4 and its availability
reason blocks Start until the artist chooses another preset. The default is never an image
sequence and is not affected by the last preset used. The default destination is
`Exports/{name}.<ext>` beside the project; an unsaved project has no default destination, and Start
says "Choose where to save this output".

### 6. The runner

- **One job at a time, in queue order; outputs of a job in order.** Concurrency is not
  configurable. The export evaluator owns one GPU device generation that admits one processing
  phase, so two concurrent jobs would split the same device and double memory without more total
  throughput. Parallelism lives inside a job: row-band CPU evaluation, multithreaded encoders
  (#369), and evaluation of frame N+1 overlapping the encode of frame N, as
  `SequenceExportRunnerV1` already does (one acknowledged product in flight).
- **Each output is its own pass** through the existing path: `beginOutputAnalysisAttemptV1` →
  policy approval (section 7) → `executeExportPublication` per frame for images, and
  `SequenceExportRunnerV1` for movies and WAV. A shared evaluation fanned out to several outputs
  was rejected: each frame's analysis digest binds one preset and one display processor, so
  fan-out would need a new multi-sink attempt graph. The operation cache makes the repeated
  evaluation of a second output cheaper.
- **Threading.** The owner calls `pump()` and the session commands. In the desktop application that
  is the UI thread's coalesced completion drain (G4A); headless callers pump from the client thread.
  `pump()` never blocks. All work is scheduler tasks at `Foreground` priority, owned by
  `TaskOwner{Export, owner}` and grouped per run. `snapshot()`, `events()` and the subscription work
  on any thread.
- **Cancellation (Stop).** Stop cancels the run's task group. A movie or WAV removes its staged
  file and leaves the destination unchanged. A sequence keeps the frames it already published,
  because each frame is its own atomic publication, and removes the staged frame. A Waiting job
  simply returns to Ready.
- **Pause** acts at a frame boundary. In-flight native GPU work cannot be pre-empted, so Pausing
  lasts at most one frame. A paused sequence holds nothing. A paused movie keeps its encoder
  session, staged file and queue byte reservation, but no GPU phase. Paused time is excluded from
  the export's no-progress and total deadlines: the runner passes a pause-aware
  `OutputExportClockV1`, and the encode session's per-request timeout applies only to requests in
  flight. Preparing and Verifying are short and cannot be paused.
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

### 7. Verification replaces the approval prompt

- **Before Start**, every output shows its preset's expected preservation and its preflight issues
  (`ExportPreflightIssue`): missing or unwritable destination, an existing file (a warning with
  Replace, blocking with Refuse), an invalid naming pattern, an estimated size above free space,
  an unavailable preset, two outputs writing the same target. Blocking issues make Start
  unavailable with the issue's reason.
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

### 8. GPU fair admission with Viewer priority

Today preview and export each own a GPU device generation, and both draw on one shared processing
root (`GpuProcessingPhasePolicy`). Each generation retains at most one processing phase, and an
unproven retirement latches it. A refused request returns an `admissionRetry` watermark and its
caller retries when that watermark moves. Under a long export that model has three problems: the
Viewer can be refused because export holds the root's bytes; a continuously busy Viewer can starve
export; and an unproven export retirement latches the export generation for every subsequent frame.

`runtime::GpuAdmissionArbiter`, constructed once by the composition root around the shared policy
and passed explicitly to the preview service and the export provider (no locator), replaces
refuse-and-retry for phase reservation:

1. **Classes.** *Viewer* (the current Viewer frame, scrub, playback and RAM-preview playback fill),
   *Export* (final render: still, sequence, movie and headless, GPU or CPU-reference host phases),
   and *Background* (thumbnails and speculative work).
2. **Viewer first.** While a Viewer request waits, no Export or Background phase is granted. A
   Viewer request never queues behind an export; the preview's one-active/one-newest gate is
   unchanged.
3. **Viewer reserve.** An Export grant must leave room on the host and the device for the last
   admitted Viewer envelope, or for the active composition's Viewer envelope before any Viewer frame
   has been admitted. Then the next Viewer frame fits while an export phase is held.
4. **Frame-sized grants.** An export phase covers exactly one frame, so export yields at every frame
   boundary.
5. **Aging.** An Export request that has waited `exportMaximumWait` (default 1 s) is granted next
   even while Viewer requests keep arriving. If the Viewer reserve cannot also be met, the Viewer
   waits for that one export frame. The bounds are therefore at least one export frame per aging
   interval during continuous playback, and at most one export frame of Viewer delay per interval.
   Background is never aged above Export.
6. **Waiting, not refusal.** An Export request waits in FIFO order with the typed status
   `WaitingForViewer` (shown as the job's device route) until a lease release revision, a fresh
   sample epoch or a quarantine retirement admits it. These are the retry signals that exist
   today, so there is no polling loop. Explicit caller caps still refuse.
7. **Sharing device time.** The export device requests LOW global queue priority through
   `VK_KHR_global_priority` / `VK_EXT_global_priority` when that capability is present. Export
   native work is already split into bounded command batches. Between batches the export owner
   thread defers its next submission while a Viewer submission is pending, for at most 8 ms per
   batch and never beyond the aging bound for a frame. Where global priority is absent (MoltenVK,
   some drivers), batch yielding alone applies.
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
   service; the arbiter only counts its held charges toward the Viewer reserve, so exports wait
   rather than starve the Viewer.

The one-phase-per-generation rule in `GpuDevice` is unchanged. The arbiter governs the root that
the two generations share; inside the export generation, one phase means one frame at a time, which
is what the serial runner does. A `--require-device` export still fails instead of falling back.

### 9. Jobs panel and Export panel

Every task a run submits uses `TaskOwner{TaskOwnerKind::Export, owner}` and one `TaskGroupId` per
run (`ExportRunLink`). The Jobs panel collapses an Export-owned group into one row. That row reads
its progress from the queue snapshot through `jobForTaskOwner()` and offers **Show in Export**.
Export-owned task rows offer no cancel, pause or retry; the Export panel owns those commands. There
is one progress source, the queue snapshot. The task snapshots still carry per-stage subphases for
diagnostics.

The panel is a registered editor type, "Export". The UI side reveals it; the engine only exposes
what it reads:

- `snapshot()`: an immutable `ExportQueueSnapshot` with a monotonic `revision`, per-job
  `revision`s for cheap diffing, job and output views, and the availability of every command at
  every level.
- `subscribe()`: an `ExportQueueSubscription` with the same neutral serial-wake shape as
  `runtime::TaskCompletionSubscription` (eight slots, stop-aware wait, idempotent close). The UI's
  adapter thread waits and posts one coalesced queued drain, which reads the snapshot and
  `events()`.
- `events(afterSequence)`: typed terminal events in a bounded ring (256), each with a sequence and
  the snapshot revision that first shows it. `ExportJobSucceeded` (with warnings or not),
  `ExportJobFailed` (with a structured `ExportFailure`), `ExportJobCancelled` (with the stop cause)
  and `ExportQueueDrained` (counts since the runner left Idle). Each job event carries its outputs'
  verification. A `gap` flag reports overflow; the snapshot still holds every result. The engine has
  no notion of sounds or notifications; a UI service may react to these events.

### 10. Commands and availability

Every command has an availability with a typed `ExportUnavailableCode` and a tooltip-ready reason,
so no control is disabled silently. Snapshot sets and the `availability(...)` queries give the same
answers. Project-truth commands are `src/commands` operations submitted on the ordinary command
path (the panel through the session, scripts in transactions). Session commands are
`ExportQueueService` methods.

| Command | Kind | Available when (otherwise the stated reason) | Script / MCP |
| --- | --- | --- | --- |
| Add to Export Queue (`AddJob`) | Project truth (undoable) | a project is open and editable, the composition exists, fewer than 1,024 jobs | `bloom.export.add-job` |
| Remove job (`RemoveJob`) | Project truth | the job is not running (Ready, Waiting or terminal) | `bloom.export.remove-jobs` |
| Remove finished (`RemoveFinished`) | Project truth | at least one job is Done or DoneWithWarnings | `bloom.export.remove-finished-jobs` |
| Duplicate job (`DuplicateJob`) | Project truth | the job exists and the queue has room; the copy has new ids and is Ready | `bloom.export.duplicate-job` |
| Move job (`MoveJob`) | Project truth | the job is not running; moving a Waiting job changes run order | `bloom.export.move-jobs` |
| Edit job: name, range, enabled (`EditJob`) | Project truth | the job is not running | `bloom.export.set-job-name`, `set-job-range`, `set-job-enabled` |
| Add output (`AddOutput`) | Project truth | the job is not running and has fewer than 16 outputs | `bloom.export.add-output` |
| Edit output: settings, destination, enabled (`EditOutput`) | Project truth | the job is not running | `bloom.export.set-output-settings`, `set-output-destination`, `set-output-enabled` |
| Apply preset (`ApplyPreset`) | Project truth | the job is not running and the preset exists (an unavailable preset may be applied; its reason then blocks Start) | `bloom.export.apply-preset` |
| Remove output (`RemoveOutput`) | Project truth | the job is not running and has more than one output | `bloom.export.remove-output` |
| Start (`Start`) | Session | not shutting down; the job is not active; the composition exists and the range resolves; at least one enabled output; no blocking preflight issue or unavailable preset on an enabled output | `export.start` |
| Start all (`StartAll`) | Session | at least one enabled job that Start would accept; jobs Start would refuse stay Ready with their reasons | `export.start-all` |
| Pause (`Pause`) | Session | the job is Rendering | `export.pause` |
| Resume (`Resume`) | Session | the job is Paused | `export.resume` |
| Stop (`Stop`) | Session | the job is active (Waiting returns to Ready; the others go to Stopping) | `export.stop` |
| Stop all (`StopAll`) | Session | at least one job is active | `export.stop-all` |
| Retry (`Retry`) | Session | the job is terminal and Start's other conditions hold | `export.retry` |
| Reveal output (`RevealOutput`) | Session | the output published at least one artifact in this session | (UI only) |

Menus: **Export → Add to Export Queue** (also on Assets composition rows) adds a job for the
composition and reveals the panel. The former Export Frame, Export Frame Range and Export
Composition items are replaced by it. A single frame or a range is chosen in the job's range, with
any preset.

### 11. Scripting, MCP and the CLI

- **Transactions** gain the `bloom.export.*` project-truth operations in the table. They have the
  same validation and undo as panel edits, and `add-job` returns the new `ExportJobId` and
  `ExportOutputId`s.
- **Session calls**, which are not transactional: `export.start`, `export.start-all`,
  `export.pause`, `export.resume`, `export.stop`, `export.stop-all`, `export.retry`,
  `export.status` (the snapshot as JSON, optionally `sinceRevision`), `export.events`
  (`afterSequence`), `export.wait` (a job or drain, with a timeout; headless only) and
  `export.presets`. MCP exposes the same set as tools, under the existing request limits.
- **One runner.** The existing `render` tool, `scripting::Render::run` and `bloom-cli render` keep
  their request shapes. They run a transient, unsaved job through the same `ExportJobRunner`, so
  every route uses the same pipeline and verification.
- **`bloom-cli export --project <file> (--all | --job <id>...)`** renders the saved queue headlessly
  and exits non-zero unless every selected job succeeds. It prints the verification as JSON. This
  is the headless route of the GPU coverage contract.

### 12. Reuse and what leaves `src/ui`

- **Moves to `src/host`** as `ExportJobRunner`:
  - the still and range driver of `FrameExportController`: plan compile, per-frame attempts, the
    single approval for a range, sequence naming, `SequenceState`;
  - the composition-export driver (`frame_export_controller_composition.cpp`);
  - the COLOR-5 deliverable colour preparation (`frame_export_controller_deliverables.cpp`);
  - the facet and failure descriptions, which become typed results;
  - `composition_export_capability.hpp`, which becomes `ExportPresetCatalogue::availabilityFor()`.
- **Reused unchanged**: `OutputAnalysisAttemptRunnerV1`, `approveFrameExportV1`,
  `executeExportPublication`, `SequenceExportRunnerV1`, `SequenceExportRetirementOwnerV1`,
  `GpuExportProvider`, `PublicationCoordinator`, `StagedArtifactCoordinator`,
  `ExportResourceLedgerV1`, `FrameRangeRunnerV1`, and the reopen verifiers.
- **Removed from `src/ui`**: `FrameExportController` and `frame_export_controller*.cpp`, and
  `composition_export_dialog.*`, whose fields become the panel's output editor. The approval and
  range dialog builders and their review captures are removed with them.
- **Stays in `src/ui`**: `H264EncoderService` (it forwards its state to the catalogue, and its
  consent and locate flow is the remedy for `EncoderNotInstalled`); the new Export editor; the
  subscription adapter; the Jobs row link; the shutdown prompt.

### 13. Linux, macOS and Windows

The model, persistence, availability, runner, events and panel are the same on every platform.
Paths use the native `std::filesystem::path` with the 1.22 locator rules. User presets live in the
platform user-data directory (XDG data, Application Support, `%APPDATA%`). Free space comes from
`std::filesystem::space`. Reveal uses the platform file manager through Qt. What varies is
reported through preset availability, never silently:

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Staged atomic publication | yes | yes | no provider today: every output reports `PublicationUnavailable` (owner question 1) |
| Movie encode worker | FFmpeg worker | AVFoundation provider (VideoToolbox) | none today: `WorkerUnavailable` (owner question 1) |
| H.264 | OpenH264 after consent, or VA-API | VideoToolbox | `WorkerUnavailable` |
| HEVC | VA-API when the driver exposes HEVC encode | VideoToolbox | `WorkerUnavailable` |
| GPU low priority for export | global priority where exposed; batch yield otherwise | batch yield (no global priority in MoltenVK) | global priority where exposed; batch yield otherwise |

macOS movie export must pass the same route verification before its presets report Available.
Until then they report `WorkerUnavailable` with the reason; today's user guide says worker export
runs only on Linux.

### 14. Issue #369 inside this release

- **End-to-end routes.** Still, sequence, video with audio, and headless/CLI are verified on a
  reference project with layers, masks, retimed video and an untagged H.264 source. Every route
  shows progress, can be stopped, and ends with a typed result naming the output location. The
  queue's verification is that result.
- **Colour tags** come from the output's colour intent (display/view, or the output colour space)
  instead of fixed BT.709. They are checked by `ColourTags` on reopen.
- **MP4 container** for H.264 and HEVC (`h264-mp4`, `hevc-mp4`), alongside MOV.
- **HEVC** only where a qualified encoder exists (VA-API, VideoToolbox). There is no software HEVC
  encoder, so other machines report `NoQualifiedEncoder`.
- **DPX sequences** (10-bit and 16-bit RGB) through the worker with a reopen verifier, carrying the
  output colour space like EXR.
- **Multithreaded encode.** The worker uses an automatic thread count. Each preset's
  `MediaDeterminismV1` states whether threading can change the bitstream. When it can, the tolerance
  profile covers decoded frames, not bytes.
- **Limits from destination capacity.** The fixed 16 GiB output cap and 8-channel audio cap are
  replaced by the destination's free space minus a safety margin, and by each container's own
  limits. Preflight refuses an estimate above free space, and the runner rechecks during the write.
- **Staged streaming publication.** The worker writes directly to the coordinator's staged file
  through an inherited descriptor, instead of returning chunks into a capped temporary copy.
- **VA-API render-node discovery** (render node plus encode entry points) is part of the encoder
  probe.

## Consequences

- Export settings survive save and reopen, and many deliverables can be prepared and run while the
  artist keeps working. The modal approval prompt disappears; its information moves to the preset's
  expected preservation before Start and to the verification afterwards.
- One runner serves the panel, scripting, MCP and the CLI. `src/ui` loses about 2,500 lines of
  export orchestration.
- Document 1.29 adds project members and allocator namespaces. Older projects open with an empty
  queue, and a 1.29 project cannot be opened by an older Bloom (same-major newer minors keep their
  additive members as today).
- The arbiter changes preview admission from refuse-and-retry to queued grants. The preview
  route proofs and the GPU coverage native acceptance must pass unchanged, with export running
  concurrently in an added witness.
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
- **Keeping refuse-and-retry admission**: it lets a long export refuse Viewer frames and lets
  playback starve exports.
- **Device priority alone**: it is not available on every platform or driver.
- **Callbacks for progress**: they break the neutral-subscription rule.
- **Saving run status or verification**: excluded by the owner, and stale after any edit.
- **Defaulting new jobs to the last-used preset**: it could make an image sequence the default,
  which the owner excluded.

## Documents updated when this is accepted

`architecture/frame-output.md` (queue, policy approval, verification), `architecture/task-system.md`
(the Export owner kind and Jobs link, shutdown with exports), `architecture/project-format.md`
(1.29), `architecture/gpu-backend.md` (the arbiter), `architecture/media-io.md` (#369),
`user-guide/exporting-video.md` (the Export panel), `user-guide/mcp.md` and
`user-guide/scripting.md` (operations and `bloom-cli export`).
