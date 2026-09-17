# Scripting Bloom with Python

The optional Python client uses the same operation registry, command stack, immutable snapshots,
task scheduler and reference render runner as the native host. Run a trusted local script with
`bloom-cli run script.py`, or start `bloom-cli python` for an interpreter with `bloom` pre-imported.
The embedded interpreter ignores `PYTHONHOME`, `PYTHONPATH`, user site packages and current-directory
imports. It loads the build's Bloom package and the selected interpreter's standard library.

## Building

Install a GIL-enabled CPython interpreter and its development headers. `BLOOM_BUILD_PYTHON` defaults
to ON when the interpreter, extension headers and embedding library are found. Explicit ON requires
all three; explicit OFF disables development-library discovery and the Python clients. MCP pipe
tests may still use a discovered Python interpreter as their test driver. Linux CI installs `python3-dev`
and enables Python explicitly. The current local qualification uses CPython 3.14.7; Linux CI's
system Python may have a different minor version. Rebuild the dependency prefix's nanobind core for
each Python SOABI. This host-interpreter policy is a development intake, not a bundled release
interpreter or a stable-ABI wheel promise. Desktop and headless builds use the same package.

The superbuild provides nanobind 3.0.1 and its separately recorded robin-map 1.4.0 subtree. Normal
application configuration performs no Python package installation or dependency download.
The build-tree package is in `build/<configuration>/python`; an external matching interpreter can
import it when that directory is explicitly placed on its import path. The install destination is
`lib/bloom/python`. `.pyi` files and `py.typed` describe the public package.

## Read data and discover operations

```python
import bloom
import inspect

composition = bloom.context.composition
print(composition.id, composition.name)
print(bloom.context.time, bloom.context.selection)
print(bloom.app.version, bloom.app.capabilities)
print(inspect.signature(bloom.ops.layer.add_solid))

for operation_id, operation in bloom.ops.registry().items():
    print(operation_id, operation.schema)
```

`bloom.data.snapshot()` returns an immutable revisioned projection. Collections such as
`bloom.data.compositions`, `bloom.data.nodes(composition)` and
`bloom.data.parameters(composition)` return stable-ID proxies. `collection.get(id)` resolves by ID;
integer indexing is only a collection position. A removed object raises `StaleObjectError`, carrying
`id` and `revision`. Assigning to a proxy or snapshot does not change the project.

Every registered operation has a callable under `bloom.ops`: hyphens become underscores, so
`bloom.layer.add-solid` is `bloom.ops.layer.add_solid`. Calls accept schema-defined keyword arguments.
Python keywords receive a trailing underscore (`bloom.asset.import` becomes
`bloom.ops.asset.import_`). IDs may be integers or live proxies. The registry has 75 IDs and eleven
implemented factories. A known operation without an adapter remains discoverable and raises `OperationError`;
its `diagnostics` retain the host code, operation and argument. Parameter animation, component keyframes and direct node removal use the owning registry
adapters shared by Python, JSON scripts and MCP. `bloom.ui`, `bloom.addons`, `bloom.props` and contribution types are reserved stubs whose
unsupported access raises `NotImplementedError` with ADR 0022 and ADR 0012 references.

## Transactions and undo

```python
with bloom.transactions.group("Build a title"):
    bloom.ops.project.set_name(name="Title study")
    bloom.ops.layer.add_text(composition=composition, name="Title", text="Hello, Bloom!")

bloom.transactions.undo()
bloom.transactions.redo()
```

The group captures an expected revision on entry. `expected_revision=` can supply an explicit
revision token. Operations are collected, validated and committed atomically as one undo entry.
An exception inside the block discards the pending operations. Nested groups are rejected.
Operations inside a group return `None`; the group's `result` becomes available after exit, with
created IDs in `outputs`. Outside a group, an operation returns its immutable command result.
Headless authoring and history calls run on the host's creating thread.

## Rendering and tasks

```python
result = bloom.render.frame(12, composition=composition, out="frame-12.png")
print(result.files[0].path, result.files[0].digest)

sequence = bloom.render.range(0, 23, out="frame.png")
```

The destination directory must already exist. Frame labels are zero-based and range endpoints are
inclusive. PNG and scene-linear float OpenEXR use the host's reference compile, analysis, approval,
publication and frame naming paths. `RenderResult` and `RenderFile` are immutable. Digests are
SHA-256 over the published file bytes. These calls release the GIL during native work; no Python
runs in the evaluator or render kernels. Native progress is available through
`bloom.tasks.snapshots()` and native task IDs can be cancelled with `bloom.tasks.cancel(id)`.

Long Python work uses a dedicated Python worker and a bounded queue:

```python
def count(task, total):
    for index in range(total):
        task.check_cancelled()
        task.report_progress(index + 1, total, "Counting")
    return total

handle = bloom.tasks.submit(count, 1000)
print(handle.result())
```

The host task scheduler observes progress and cancellation without executing the Python callable
on a native worker. `handle.cancel()` requests cooperative cancellation. The client traces Python bytecode for cancellation; native calls must still cooperate
and return between bounded slices. Complete persistent edits on the authoring thread after
retrieving the result. An in-process native extension that hangs cannot be forcibly cancelled
safely. Shutdown stops queued work and waits for running trusted work to finish.

## Events

```python
with bloom.events.subscribe(lambda event: print(event["kind"], event["revision"])):
    bloom.ops.project.set_name(name="Observed")
    bloom.events.pump()
```

Subscriptions are explicitly closeable. Native callbacks enqueue values; a Python callback runs
only when the thread that subscribed calls `pump()`. The pump is bounded and does not run callbacks
inline on a render or scheduler worker. Native and subscriber queues each retain 1024 events. If
either overflows, the next pump delivers `{"kind": "reset"}` before retained events, regardless of
the subscription's kind filter; re-query the snapshot when that happens. Native session access is
serialized while rendering or saving, and waiting readers release the GIL so another thread can
still observe task progress or request cancellation.

## The Script editor

Choose **Script** in any editor area's panel switcher. Enter a Python statement and press
**Ctrl+Enter** (also the numeric keypad Enter), or click **Run**. `bloom` is pre-imported and variables
persist for the application session. The history dropdown recalls the last 32 inputs. Use
`exec("line one\nline two")` for a block, or run an existing trusted script with Python's file APIs.
Output and tracebacks appear in the panel. Multiple Script areas share the interpreter; closing an
area leaves its work running. Python-disabled builds show an unavailable explanation in this editor.

`bloom.context` follows the live project, composition, selection and time. A transaction enters the
UI command queue and commits on the UI thread, at most one queued transaction per pump slice.
The normal **Edit → Undo** action reverses a whole Python transaction. Expected-revision conflicts
leave the current document intact. Project New/Open invalidates old bindings and proxies and cancels
pending work; the next input binds to the newly installed project. Use the UI Save command to save
this live document.

For example, with a layer selected, animate its position X:

```python
composition = bloom.context.composition
node = bloom.data.nodes(composition).get(bloom.context.selection[0].id)
parameter = node.parameters["position"]
with bloom.transactions.group("Key position"):
    bloom.ops.animation.create_for_parameter(
        composition=composition, parameter=parameter, time=(0, 1))
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
        composition=composition, parameter=parameter, component=0,
        time=(1, 2), value=400.0)
```

Use the parameter roles in the node's `parameters` projection; some selected nodes do not own a
position parameter. Component IDs follow the host's semantic enum: X=0, Y=1, Z=2, Red=3, Green=4,
Blue=5, Alpha=6. See `examples/scripting/keyframe.py` for a selection-independent example.

Python runs on a dedicated scripting thread; the UI thread only executes queued host transactions.
**Cancel** stops queued scripts, requests native task cancellation, and interrupts traced Python
work. Foreground GUI execution has a two-second Python-thread CPU budget; put longer computation
or renders in `bloom.tasks.submit`. The footer displays native task progress. Each input is bounded
to 65,536 characters, pending input/transaction queues to 16 entries, and retained output to 262,144
characters. The console runs trusted local code; tracing is a responsiveness measure, not isolation
from malicious Python or a native extension that never returns.
