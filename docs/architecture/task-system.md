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

## Implemented Kernel Checkpoint

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

This kernel does not yet decide whether a result may update Viewer or another panel. Batch 2 owns
the Qt bridge, Jobs model/editor, snapshot compiler, and composition-preview controller that attach
document revision and request generation, suppress obsolete publications, and coordinate panel,
project, and application lifetime. No current UI surface submits pixel evaluation to the kernel.

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

Revision and generation acceptance are application-controller semantics, not scheduler policy. The
implemented kernel transports typed results and terminal state without knowing document revisions;
the Batch 2 preview controller must perform these publication checks on the UI side of the neutral
mailbox boundary.

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

The kernel provides structured snapshots and group aggregation. Rate-limited Qt delivery and visual
presentation belong to the deferred Batch 2 bridge and Jobs model.

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

The implemented `beginShutdown` boundary covers prompt admission closure and cancellation request.
Application-owned quiescence observation, continued Qt event processing, save/export policy, and
ordered destruction of the bridge and runtime remain integration work; a panel destructor must never
own that drain.

## Testing And Instrumentation

Kernel tests cover priority and fairness, executor separation, bounded admission, coalescing,
reprioritization, cancellation before and during work, task groups, progress, diagnostics, exception
conversion, terminal/result coherence, bounded registries and history, and shutdown initiation.

Batch 2 and later integration tests still need to cover:

- stale revision and stale request-generation suppression
- panel and project destruction while result delivery is pending
- rate-limited Qt progress delivery and Jobs diagnostics
- partial-output cleanup and atomic publication
- simulated device loss and fallback policy
- staged application shutdown with active preview, save, and render tasks

UI integration tests use deliberately slow fake operations and assert that event processing, input,
and cancellation remain responsive. Thread sanitizers, lock-order checks, queue-depth metrics, task
latency, cancellation latency, and UI frame/event latency become part of performance diagnosis as
the implementation grows.

## Implemented Boundary And Next Integration

The bounded executors, task groups, cancellation, priority, progress, diagnostics, terminal
outcomes, and neutral non-blocking result delivery are implemented in the runtime kernel. The next
integration must add generation/revision checks for Viewer requests, queued delivery through a Qt
application service, the Jobs surface, and clean project/application quiescence without moving
those policies into the scheduler.

Distributed rendering, persistent job databases, and a general workflow engine are not required.
The contract above is intentionally broader than the first implementation so later media and GPU
work cannot bypass the responsiveness boundary.
