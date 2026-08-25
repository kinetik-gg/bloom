# Task System And UI Responsiveness

Status: working

Updated: 2026-08-25

## Objective

Bloom must remain interactive while it decodes media, evaluates graphs, renders frames, builds
caches, compiles GPU work, saves projects, or performs any other potentially expensive operation.
Moving a function to a worker thread is not sufficient by itself: work also needs bounded resource
use, cancellation, progress, diagnostics, priority, and a safe way to publish results.

The task system is an application service. It is independent of panels and outlives individual UI
objects. Panels submit requests and observe state; they do not own worker threads.

## Implemented Runtime And Qt Checkpoint

The Qt-free `src/runtime` kernel currently implements:

- independently bounded CPU and blocking-I/O executors with bounded worker, queue, history, and task
  group registry configuration
- stable task/group IDs, typed owner scopes, four semantic priorities, fair scheduling,
  reprioritization, and transactional coalescing admission
- cooperative task and group cancellation; queued cancellation finalizes off the caller and releases
  queue capacity promptly, while running work observes cancellation through its token
- structured progress, diagnostics, terminal snapshots, task-group aggregation, and typed
  non-blocking result handles delivered through neutral mailboxes
- admission shutdown that returns promptly, rejects new work, requests cancellation, and leaves
  quiescent service destruction to its explicit lifecycle owner

The implemented Qt/application layer adds:

- a rate-limited UI-thread bridge that polls bounded value snapshots without registering worker
  callbacks or retaining Qt objects in runtime work
- one shared task-monitor model and disposable Jobs panels with stable-ID cancellation and complete
  structured diagnostic presentation
- a frozen node-definition registry and deterministic compiler that consumes immutable document
  snapshots and emits typed composition plans without allocating images
- a bounded CPU composition evaluator that preflights process-image memory, evaluates immutable
  plans, cooperatively cancels at operation and scanline boundaries, and publishes a process-only
  immutable frame
- a separate CPU reference-display preparer with its own typed frame, identity, diagnostics,
  progress, exact aggregate process-plus-display budget, and no-partial-publication cancellation
- one scheduled preview task per generation that runs compile, evaluate, and display preparation
  sequentially without nested task submission or waits
- an application-owned preview controller that accepts results only for the exact project,
  composition, revision, request generation, time, resolution, quality, and color intent
- exact `CompositionSession` rational time as an evaluation-request input, outside project truth,
  dirty state, and undo history
- a one-active/one-newest-pending preview gate: superseding intent immediately advances generation,
  cancels but retains the active handle until terminal, and replaces rather than accumulates pending
  work
- separate preview activity and frame-freshness state, with the last good same-composition frame
  retained during rendering, cancellation, unsupported outcomes, and failures
- an extracted Viewer that borrows immutable packed RGBA8 only during paint, fits pixel aspect in
  logical widget coordinates, and labels the temporary path `Reference display`
- staged shutdown for window and application quit paths, with one stable terminal task publication
  before runtime quiescence permits process exit

`Ready` now means that a `PreparedPreviewFrame` containing a `ProcessFrame` and separately
identified `ReferenceDisplayFrame` matches the controller's current desired publication identity.
It never means merely that a plan compiled. Process and display identities remain independent cache
boundaries. Qt types remain outside the task, compiler, evaluator, display preparer, and frame
contracts.

## Responsiveness Contract

The Qt UI thread may:

- process input and Qt events
- update session state and issue commands
- capture a cheap immutable document snapshot or snapshot handle
- submit, reprioritize, coalesce, or cancel task requests
- consume already-completed results
- perform bounded presentation calls required by Qt or the window system

The Qt UI thread must not:

- decode, encode, probe, hash, or scan media
- evaluate graphs or render frames
- generate thumbnails, proxies, waveforms, or cache entries
- compile shaders or GPU pipelines
- allocate or transfer large image resources
- wait on GPU fences, worker threads, processes, futures, condition variables, or task completion
- perform project serialization, compression, or other unbounded file I/O
- synchronously drain a task queue during panel destruction or application shutdown

A call is not UI-safe merely because it is usually fast. If latency depends on media size, project
size, cache state, driver state, filesystem state, network state, or an external process, treat it as
asynchronous. Small, bounded document validation and command application may stay synchronous so
interactive edits remain coherent.

## Request And Outcome Contract

Every non-trivial request carries:

- a stable task ID and human-readable operation name
- an owner scope, such as application, project, composition, panel request, or export job
- a priority class
- a cancellation token
- a progress channel
- a diagnostic channel
- the immutable inputs and document revision it evaluates
- an explicit result or terminal outcome

The common lifecycle is:

```text
created -> queued -> running -> succeeded
                         |----> cancelled
                         |----> failed
```

Cancellation is an expected outcome, not an error. Failures contain structured diagnostics rather
than only log text or exceptions. Task IDs remain useful after completion so the UI, logs, and
support reports can correlate a visible operation with its work.

## Immutable Inputs And Result Publication

Background tasks never retain mutable document objects, Qt widgets, or `QObject` references.
Evaluation and render work consumes an immutable document snapshot plus an explicit request that
includes time, output, resolution, quality, and color intent.

Each result identifies the source revision and request generation. The receiving application
service publishes it only if:

- its owner scope still exists
- the consumer still wants that request generation
- the source revision is still valid for the result's purpose
- the task reached a successful terminal state

An obsolete preview may populate a content-addressed cache when its key remains valid, but it must
not replace a newer frame in the viewer. Persistent operations such as save and export use their
captured snapshot deliberately; later edits do not silently change the operation already in flight.

Frame-output approval uses an application-owned pre-approval task group rather than deferring
evaluation until after the decision. The group evaluates or reuses one immutable process frame,
prepares its semantic identity in a cancellable CPU `Identifying` stage, and publishes the bounded
owning products from which analysis and its exact digest are derived. The application service—not a
panel—retains and charges those products while an artist decides. Approval constructs an immutable
export request that retains the same products; export cannot reevaluate or substitute the frame,
processor, report, or identity. Dismissal or supersession releases the attempt when no admitted job
retains it. The complete ordering and ownership contract is defined by
[`frame-output.md`](frame-output.md).

Revision and generation acceptance are application-controller semantics, not scheduler policy. The
kernel transports typed results and terminal state without knowing document revisions; the
implemented preview controller performs these publication checks on the UI side of the neutral
mailbox boundary.

Pixel cache identity is distinct from publication identity and split by product. Process-cache
identity includes the complete immutable plan, rational time, output, resolution, process quality,
provider, evaluator and image-primitive versions, and only color configuration/context inputs used
by process operations. It excludes display/view, looks, monitor/output intent, packing, and display
processor. Display-cache identity begins with the exact process-frame identity and adds every
display/view, look, context, monitor/output intent, prepared-processor, packing, and display-mapper
version that can change display pixels. The caches have separate bounded budgets; a combined
preview cache key is forbidden. Request generation, cancellation state, priority, and memory budget
do not change pixels and therefore enter neither key.

## Scheduling And Priority

The initial scheduler has four semantic priority classes:

1. `interactive` — direct manipulation, scrub, and the next playback frame
2. `visible` — work needed by an on-screen editor, such as the current viewer frame
3. `foreground` — explicit user jobs with visible progress, such as save or export
4. `background` — thumbnails, proxies, indexing, cache warming, and speculative work

Priority expresses user intent, not a dedicated thread per class. The scheduler may reserve
capacity for interactive work, but it must also apply aging or fair sharing so continuous scrubbing
does not starve an explicit export indefinitely.

New interactive requests coalesce or supersede equivalent queued requests. For example, scrubbing
through twenty times should not require evaluating nineteen frames the user no longer wants.
Already-running work is cancelled when safe or allowed to finish without publishing stale results.

The preview controller now bounds high-frequency session changes above the scheduler with at most
one retained active handle and one newest pending request per preview owner. Current-time changes
advance desired generation immediately. A superseded active request is cancelled but remains the
gate until terminal; only then is the newest pending immutable request submitted. Scheduler
coalescing, cancellation, and revision/generation publication checks remain mandatory backstops.

The accepted interaction contract additionally requires an injectable 16 ms trailing cadence for
pointer-driven scrub and direct-manipulation storms. Gesture end bypasses that delay for the newest
desired request without bypassing the active-request gate. The cadence, timeline projection, and
direct-manipulation request source remain pending; this document does not report them as live merely
because the active/pending gate is implemented.

Tasks declare the resources they need where practical: CPU, blocking I/O, GPU device/queue, external
process, and estimated memory. Executors use bounded queues, concurrency limits, and memory budgets.
Submitting work must apply backpressure or discard replaceable speculative requests instead of
growing an unbounded queue.

The implemented kernel currently distinguishes bounded CPU and blocking-I/O execution. GPU queues,
external processes, and memory-budget admission remain owning-adapter responsibilities for later
batches; they must preserve the same cancellation, diagnostics, and no-UI-wait contract.

## Cancellation

Cancellation is cooperative and transitive:

- task groups propagate cancellation to child tasks
- long loops and stage boundaries check their token
- decoders, render nodes, and external integrations receive cancellation when their APIs allow it
- a non-interruptible library call is isolated so it cannot block the UI or task coordinator
- partial cache or output writes are not published as complete

Tasks do not promise immediate cancellation when a third-party operation cannot be interrupted.
The UI must nevertheless acknowledge the request immediately, stop waiting for that result, and
remain responsive. Atomic output staging or cleanup prevents cancelled saves and renders from
masquerading as valid artifacts.

## Progress And Diagnostics

Progress is structured and rate-limited before reaching the UI. A task reports:

- current phase and optional subphase
- completed and total work when the total is meaningful
- indeterminate state when it is not
- whether cancellation has been requested
- warnings and errors with stable codes, severity, source context, and suggested action where known

Nested work aggregates through task groups. Progress is monotonic within a phase, but a phase may
change its estimate as new work is discovered. The UI must not infer success from reaching 100%;
only the terminal outcome is authoritative.

The kernel provides structured snapshots and group aggregation. The implemented Qt bridge polls at
a bounded rate, and the shared Jobs model presents textual progress without turning worker reports
into an unbounded UI event stream.

Diagnostics flow through an application-level sink. Workers do not open dialogs, touch status-bar
widgets, or emit presentation-specific strings directly. The same diagnostic can therefore serve a
panel, task monitor, headless renderer, log, or test.

## CPU, I/O, And GPU Execution

CPU computation and blocking I/O use bounded executors with separate concurrency controls so slow
storage cannot consume all compute capacity. The design does not require one operating-system
thread for every task.

GPU work goes through the render backend and its device/queue owners. Potentially blocking resource
creation, uploads, pipeline compilation, fence waits, and readback do not run on the UI thread.
Presentation may use a short platform-required UI-thread handoff, but it consumes prepared resources
and does not wait for their production.

Executors and task primitives belong below the Qt boundary. `src/ui` adapts task state into Qt
models and delivers completion through queued UI-thread dispatch. Core task APIs do not expose
`QThread`, `QFuture`, `QPromise`, Qt containers, signals, slots, or `QObject` lifetime semantics.

Host-side color resource validation, hashing, and processor preparation are blocking-I/O stages.
The immutable qualified Bloom Neutral built-in may construct and apply its processor in-process
only after the bounded cross-platform gates in [`color-management.md`](color-management.md) pass.
Every project-relative or external config delegates all OCIO parsing, processor construction, and
transform application to a supervised, memory-limited `bloom-color-worker`. The owning blocking-I/O
task drives bounded IPC and monotonic deadlines directly; it does not wait on another Bloom task or
worker. Cancellation and shutdown terminate an unresponsive helper within the accepted deadline,
invalidate its opaque processor tokens, reclaim its IPC slabs, and publish no partial product.

The application controller submits each dependent color stage only after receiving its
predecessor's typed successful result. An in-process built-in applies a prepared processor in
bounded CPU chunks; a helper-backed stage exchanges distinct bounded shared-memory slabs. Neither
path nests task submission, blocks the UI event loop, or exposes OCIO objects through task results.

Project saves and frame exports share one `src/platform::StagedArtifactCoordinator`. Canonical
target preflight and no-follow inspection run on blocking I/O. Accepted operations receive monotonic
publication-intent IDs. Before atomic publication, an older save or export is `Superseded` when a
newer intent owns the same canonical target, so worker completion order cannot reverse artist
intent. Each move-only `StagedArtifactLease` pins the canonical parent identity, rejects a
symlink/reparse or non-regular final target, and revalidates the observed target fingerprint before
atomic replacement.
An external change is a typed conflict, never implicit overwrite permission. The detailed lease,
cleanup, and publication outcomes are defined by
[`frame-output.md`](frame-output.md) and [`project-format.md`](project-format.md).

## Device Loss And Runtime Recovery

The GPU backend treats device loss as a recoverable runtime state, not a UI-thread exception:

1. stop accepting work for the failed device
2. cancel or fail its queued and in-flight task groups with structured diagnostics
3. invalidate device-owned derived resources while preserving project truth
4. attempt device recreation asynchronously when supported
5. resume from immutable requests or offer the CPU path when it can satisfy the operation

Preview may fall back or reduce quality only with a visible degraded-state indicator. Final render
must follow its declared policy: use a validated equivalent backend or fail clearly; it must never
silently change quality or color behavior after device loss.

## Shutdown And Lifetime

Shutdown is a staged asynchronous operation:

1. stop accepting ordinary new work
2. cancel replaceable preview and background groups
3. protect or explicitly resolve active save and export operations
4. detach panels from task observations
5. drain or terminate executors outside panel destructors
6. destroy runtime services only after callbacks can no longer target application state

Closing a panel cancels its scoped requests without joining workers. Closing a project prevents its
late results from publishing. Application shutdown keeps processing UI events while it presents any
required save/export decision; it does not freeze the event loop waiting for a join. No incomplete
project write or final output is reported as successful.

The implemented composition root intercepts both window-close and application-level quit, closes
admission, cancels replaceable preview work, continues processing Qt events, and permits process
exit only after the bridge has published stable terminal snapshots and observed runtime quiescence.
Panels never own that drain. Save/export resolution policy remains future work because those task
kinds do not yet exist.

## Testing And Instrumentation

Kernel tests cover priority and fairness, executor separation, bounded admission, coalescing,
reprioritization, cancellation before and during work, task groups, progress, diagnostics, exception
conversion, terminal/result coherence, bounded registries and history, and shutdown initiation.

Integration tests now cover stale revision and same-revision generation suppression, selection-only
changes, Viewer and Jobs replacement during live work, rate-limited UI-thread progress delivery,
ordered diagnostics, exact session-time publication, one-active/one-newest request storms, stable
terminal publication, application-level quit interception, and staged shutdown with cooperative
preview work.

Later batches still need to cover project destruction, partial-output cleanup and atomic
publication, simulated device loss and fallback policy, and shutdown decisions for active save and
final-render tasks.

UI integration tests use deliberately slow fake operations and assert that event processing, input,
and cancellation remain responsive. Thread sanitizers, lock-order checks, queue-depth metrics, task
latency, cancellation latency, and UI frame/event latency become part of performance diagnosis as
the implementation grows.

## Implemented Boundary And Next Integration

The bounded executors, snapshot compiler, revision/generation-safe preview controller, exact session
time, one-active/one-newest preview gate, Qt bridge, Jobs surface, clean application quiescence, and
Batch 3 CPU preview path are implemented. The
immutable typed plan is consumed by a deterministic CPU evaluator that publishes real pixels
without weakening the established ownership, cancellation, color, alpha, or stale-result
contracts. The 16 ms pointer cadence, timeline key projection, and direct manipulation remain
pending interaction work.

Distributed rendering, persistent job databases, and a general workflow engine are not required.
The contract above is intentionally broader than the first implementation so later media and GPU
work cannot bypass the responsiveness boundary.
