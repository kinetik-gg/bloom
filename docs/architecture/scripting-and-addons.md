# Scripting And Add-ons

Status: accepted direction

Updated: 2026-08-25

## Purpose

Bloom exposes Python as its one general-purpose language for artist automation, studio pipelines,
headless scripting, and third-party add-ons. The scripting boundary is designed with the document,
command, task, editor, node, and project-extension boundaries rather than added as direct access to
internal C++ objects later.

Choosing Python defines the public ecosystem language. It does not move project truth, render
semantics, or latency-variable work into Python, and it does not pull the public SDK into the first
composition proof.

## Language And Compatibility

- Python is Bloom's only core scripting and add-on language.
- Follow the Python minor version selected by the supported VFX Reference Platform generation. The
  initial CY2026 target is CPython 3.13.x.
- Bloom bundles and pins its own interpreter, standard library, private native bridge, and Python
  facade. It never links to or discovers an arbitrary system Python at runtime.
- Pin an exact Python patch release in each Bloom build manifest while publishing compatibility at
  the declared minor-version boundary.
- Lua is not a second Bloom API. An optional future pipeline may own a narrowly scoped Lua runtime
  when a concrete domain requires it.
- General Python and Lua are both excluded from per-pixel kernels, GPU shaders, realtime audio
  callbacks, render scheduling inner loops, and deterministic expression evaluation.

The current platform reference is the
[VFX Reference Platform](https://vfxplatform.com/), which specifies Python 3.13.x and Qt/PySide
6.8.x for CY2026.

## Extension Tiers

Bloom has explicit compatibility and trust tiers:

| Tier | Use | Compatibility and trust |
| --- | --- | --- |
| Python core API | data reads, commands, transactions, tasks, project I/O, events, headless automation | versioned Bloom API; no Qt or mutable C++ pointers |
| Host-rendered add-on UI | actions, menus, tools, property sections, editor descriptions, task views | stable schema-driven API; Bloom owns widgets and styling |
| Custom PySide UI | complete `QWidget` panels and editor areas | trusted, GUI-only, exact Bloom/Qt/Python release, restart may be required |
| Native C++ module | render/evaluation services or deeply native integration | source-built first-party by default; public binary SDK remains a separate future product |
| Isolated add-on process | third-party, crash-prone, untrusted, or dependency-conflicting tools | versioned DTO/RPC protocol; sandbox claims require actual OS enforcement |

The ordinary public add-on path is Python plus host-rendered UI. A full custom widget is supported as
an advanced tier, not as the compatibility baseline every add-on must pay for.

## Language-neutral Host Boundary

Python consumes the same owned application boundaries as the UI and headless frontend:

```text
immutable read snapshots
typed command gateway and transactions
cancellable task/future service
typed event subscriptions
typed contribution registries
diagnostics and capability reporting
```

These contracts use stable IDs, revisioned values, explicit outcomes, and serializable request or
result objects. They do not expose Qt widgets, mutable document containers, renderer internals, raw
C++ pointers, GPU handles, or a global service locator.

## Public Python Model

Bloom adopts the clarity of Blender's `context`, `data`, `ops`, and registerable-type model while
preserving Bloom's asynchronous and command-based ownership rules. The resemblance should help an
artist guess where an API lives; it is not a requirement to reproduce every behavior or
context-dependent edge case.

This is behavioral and conceptual inspiration only. Bloom must not copy Blender source, add-on
implementation code, generated bindings, bundled assets, or other GPL-licensed implementation
material. API contracts and examples in this repository are independently authored from Bloom's
own document, command, task, and extension architecture. Any third-party dependency or imported
asset still passes license and provenance review before entering the source tree or distribution.

The initial conceptual Python surface is:

| Namespace | Purpose | Persistent mutation |
| --- | --- | --- |
| `bloom.context` | current project, composition, selection, time, active area, and GUI/headless capabilities | none; transient execution view only |
| `bloom.data` | immutable projections, collections, queries, and stable-ID lookup | none; assignment sugar may only submit an operation |
| `bloom.ops` | discoverable, typed artist operations grouped by domain, such as `layer`, `node`, `project`, and `render` | yes, through the command gateway and undo system |
| `bloom.transactions` | group several operations into one validated atomic undo step | yes, on commit |
| `bloom.types` | base types and descriptors for operators, panels, nodes, properties, importers, and other contributions | registration only |
| `bloom.props` | declarative property and schema definitions | registration only |
| `bloom.tasks` | cancellable asynchronous work, progress, and diagnostics | only through explicit operation continuations |
| `bloom.render` | immutable render requests, results, and task handles | no direct document mutation |
| `bloom.ui` | schema layouts, icons, presentation state, and gated advanced UI facilities | presentation state only |
| `bloom.addons` | registration lifecycle, descriptor, permissions, capabilities, and API-version information | owner-scoped registration only |
| `bloom.app` | build, version, platform-capability, path-role, and runtime information | none |

The intended artist experience is compact even though the host machinery is strict. Exact names
remain subject to the API spike, but the accepted usability level looks like this:

```python
import bloom

composition = bloom.context.composition
title = bloom.ops.layers.add_text(composition=composition, text="Hello, Bloom!")

with bloom.transactions.group("Build title"):
    bloom.ops.parameters.set(title, "transform.position", (960, 540))
    bloom.ops.parameters.set(title, "opacity", 0.9)
```

`bloom.ops` is the public operation vocabulary. Internally, each persistent operation produces one
or more typed C++ commands and follows the same validation, authoring-thread, undo, diagnostics,
and event path as an equivalent UI action. The lower-level command implementation is not a second
competing public mutation API.

Operators receive their execution `context` as an argument and should prefer it over ambient
`bloom.context` state. Ambient context remains convenient for a console or short artist script.
Every durable operation also accepts explicit project, composition, and object targets for
deterministic headless and pipeline use. Bloom should not inherit an API where an operation succeeds
only because a particular area happens to be focused.

Namespace organization is discoverability, not permission enforcement. The host authorizes every
operation at the command, task, file, network, render, and UI gateway using the current script or
add-on scope. A known but unavailable operation remains inspectable and returns a structured
capability or permission error. Scripts can query the same requirements before presenting an
action.

This follows Blender's useful distinction between
[`bpy.context`](https://docs.blender.org/api/current/bpy.context.html),
[`bpy.data`](https://docs.blender.org/api/current/bpy.data.html), and
[`bpy.ops`](https://docs.blender.org/api/current/bpy.ops.html), while avoiding implicit focused-area
requirements for operations that can be expressed with explicit targets.

## Stable Proxies, Commands, And Undo

Python objects are stable-ID proxies resolved through the host each time. Deletion, project close,
or generation mismatch produces a structured stale-object error rather than a dangling reference.

Reads use immutable revisioned snapshots. Mutations build typed commands or an explicit transaction
and commit through the same validation and authoring thread as the UI. Python does not retain a
document lock while running or awaiting work.

An ergonomic property wrapper may eventually expose assignment-like syntax, but canonical methods
must preserve asynchronous validation and structured failure. API v1 `bloom.data` proxies are
read-only; durable mutation occurs through `bloom.ops` and transactions. Assignment-like mutation
is not part of the initial compatibility promise. A multi-step transaction produces one meaningful
undo entry and restores exact records, IDs, edges, values, and order.

## Runtime And Thread Ownership

The first trusted runtime uses one normal GIL-enabled CPython interpreter configured in isolated
mode and owned by a dedicated scripting thread.

- Ignore ambient `PYTHONHOME`, `PYTHONPATH`, user site packages, and current-directory imports.
- Set explicit bundled standard-library, Bloom package, and managed add-on paths.
- Do not execute ordinary Python callables on the UI thread, CPU worker pool, render threads, GPU
  queue threads, or platform callbacks, except for the explicitly gated Custom PySide UI tier.
- Release the GIL around native latency-variable work and return Bloom task/future handles.
- Post completion back to the scripting loop; never invoke Python inline from a worker completion.
- Finalize only after new scripting work is stopped, callbacks are revoked, tasks are quiesced, and
  Python-authored UI is destroyed while Qt is still alive.

Free-threaded CPython and subinterpreters remain later qualification work. They do not replace
Bloom's immutable ownership, queue, backpressure, and task contracts and are not security
boundaries. CPython documents both embedding and isolated initialization through its
[embedding guide](https://docs.python.org/3/extending/embedding.html) and
[`PyConfig` API](https://docs.python.org/3/c-api/init_config.html).

A hung in-process Python script or native extension cannot be killed safely. Trusted console work
may require application termination when it refuses cooperative cancellation. Third-party work
that needs hard cancellation belongs in an add-on process.

## Binding Stack

The working implementation choice for the first cross-platform spike is:

- CPython 3.13.x embedding and lifecycle through the CPython C API
- pybind11 3.x for `_bloom`, the private C++ binding module
- a typed pure-Python `bloom` facade above `_bloom`
- checked and shipped `.pyi` stubs plus `py.typed`

pybind11 is selected for its first-class embedding API, RAII, CMake integration, licensing, and
maturity. `_bloom` ships with one exact interpreter and does not need to be a general stable-ABI
wheel. Rebuild it on intentional Python-minor upgrades.

Nanobind may be reconsidered for separately distributed native modules where its limited-API
support matters, but embedding is explicitly outside its current project scope. Boost.Python does
not provide a practical advantage for a new C++20 host.

Primary references:

- [pybind11 embedding](https://pybind11.readthedocs.io/en/stable/advanced/embedding.html)
- [nanobind rationale](https://nanobind.readthedocs.io/en/latest/why.html)
- [CPython stable ABI](https://docs.python.org/3/c-api/stable.html)

## Standard Add-on Packaging

Use Python packaging standards wherever they cover the requirement:

- wheel distributions
- `pyproject.toml`
- standard Python version, dependency, license, and platform metadata
- a namespaced entry-point group such as `bloom.addons.v1`

For example:

```toml
[project]
name = "vendor-bloom-tools"
requires-python = ">=3.13,<3.14"

[project.entry-points."bloom.addons.v1"]
"vendor.tools" = "vendor_bloom_tools.addon"
```

PyPA defines [wheels](https://packaging.python.org/en/latest/specifications/binary-distribution-format/)
as the binary distribution format and
[entry points](https://packaging.python.org/en/latest/specifications/entry-points/) as the mechanism
for installed distributions to advertise plugin components.

The Bloom manifest owns the stable add-on ID, host API range, permissions, execution tier, and
project schemas. The PyPA entry point owns only Python package discovery. Its entry-point name must
equal the manifest add-on ID, and the manifest refers to that name rather than repeating a Python
import path. The validator rejects disagreement before import.

Bloom-specific data still needs a small, versioned `bloom-addon.toml` manifest because Python
metadata does not describe Bloom host API ranges, permissions, project schemas, execution tier, or
contribution types. Its schema and wheel placement are frozen by the packaging spike, but it must
be inspectable without importing or executing the add-on.

The manifest contains at least:

- schema version, stable namespaced add-on ID, and add-on version
- supported Bloom API range
- entry point, execution tier, and headless policy
- supported platforms and architectures
- required and optional add-ons
- Python/native dependency inventory and hashes
- license, maintainer, documentation, source, and support links
- provided contribution categories
- requested access with a short human-readable reason
- namespaced project-data schema versions

Opening a project may report a required add-on but never installs, downloads, enables, imports, or
executes it automatically.

## Canonical Add-on Project Shape

An installable source project has one unambiguous root containing both `pyproject.toml` and
`bloom-addon.toml`:

```text
vendor-bloom-tools/
  pyproject.toml
  bloom-addon.toml
  README.md
  LICENSE
  src/
    vendor_bloom_tools/
      __init__.py
      addon.py
      operators.py
      panels.py
  tests/
```

Only `pyproject.toml`, `bloom-addon.toml`, and the declared import package are required. The manifest
names an entry point from `bloom.addons.v1`; the entry point resolves to a normal Python module with
a `register(registrar)` callable and optional `unregister(registrar)` cleanup hook. A minimal
`addon.py` stays recognizable to Blender add-on authors:

```python
from .operators import NormalizeTransforms
from .panels import PipelinePanel

CLASSES = (NormalizeTransforms, PipelinePanel)


def register(registrar):
    registrar.register(*CLASSES)
```

The one scoped registrar accepts registerable `bloom.types` declarations and also has explicit
helpers such as `operator`, `panel`, `node`, and `importer`. The manifest add-on ID automatically
scopes local contribution IDs: for example, `normalize_transforms` becomes
`vendor.tools.normalize_transforms`. Import may define descriptors but never registers or mutates
Bloom through top-level side effects.

Registration is transactional. Bloom records every contribution, subscription, and task group
under the add-on owner's lease and revokes them if registration fails or the add-on is disabled.
The optional `unregister(registrar)` hook only releases external resources that Bloom does not own;
authors do not manually reverse every registered class. In-process and process-hosted add-ons
receive the same registrar facade, while a non-serializable contribution fails validation when its
execution tier cannot support it.

Registration and unregistration callbacks must be quick and deterministic. They declare classes,
factories, schemas, and subscriptions; startup I/O or preparation is scheduled through the task
service after registration. Blender's official API likewise separates module import from
[`register()` and `unregister()`](https://docs.blender.org/api/current/info_overview.html#registration).

`bloom-addon.toml` is a static, versioned host manifest. `pyproject.toml` remains the source of
standard package metadata. The first SDK spike freezes the exact manifest keys and wheel placement,
then ships a template and validator so authors do not hand-assemble distribution metadata.

## Install Sources And Root Discovery

The Add-ons surface accepts four equivalent inputs:

- a built `.whl`
- a ZIP or standards-compliant source distribution
- a local project folder
- a Git repository URL, with an optional revision and subdirectory

Every input is copied or cloned into an inert staging area and then converges on the same static
inspection, validation, preview, and explicit-enable pipeline. A built wheel skips source-root
discovery and building. Selecting a local folder takes a snapshot by default. An explicit
`Development link` mode may watch the live folder, is marked trusted and mutable, and revalidates
whenever its source changes.

Root discovery is deterministic:

1. Use the selected root when it contains exactly one `pyproject.toml` plus `bloom-addon.toml` pair.
2. Otherwise ignore known metadata, virtual-environment, build, and version-control directories and
   scan to a small documented depth.
3. If exactly one valid pair is found, select it automatically. A ZIP containing one wrapper folder
   therefore works without user cleanup.
4. If several valid projects are found, show their manifest names and relative paths and require a
   choice. Never guess between add-ons in a monorepo.
5. If none are found, report both required filenames and the paths inspected. A legacy
   `setup.py`-only tree is not a Bloom add-on.

An explicit Git/source `subdirectory` bypasses scanning after validating that it stays within the
staged root. This matches Python packaging's standard model for a source tree and its
[direct-URL subdirectory](https://packaging.python.org/en/latest/specifications/direct-url-data-structure/)
field.

Git ingestion runs in a hardened external worker. Bloom accepts documented URL schemes, clones no
submodules, runs no repository hooks or credential prompts in the UI process, disables checkout
filters, and does not initialize executable code during discovery. A branch or tag is resolved to a
commit; Bloom records the requested source, resolved commit, selected subdirectory, and content
hash. Updating means staging and approving a new resolved generation, never silently following a
moving branch.

Archive ingestion rejects absolute paths, parent traversal, escaping links, duplicate normalized
paths, excessive file counts, excessive expanded size, and suspicious compression ratios. It does
not recursively unpack arbitrary nested archives. Source builds happen only after static metadata
and access review, in an isolated external worker with network access denied by default. The
[Python packaging specification](https://packaging.python.org/en/latest/specifications/source-distribution-format/)
defines a modern source tree by its `pyproject.toml`; build hooks are executable and are therefore
never part of Bloom's passive parser.

## Installation And Environments

Installation and activation are separate:

```text
select wheel / ZIP / folder / Git -> stage inert source -> discover root -> inspect
    -> validate/build -> show source, trust, access, and changes
    -> install generation -> explicit enable -> import entry point -> register -> start
```

- Install into a staging generation through an asynchronous external-process task.
- Preserve source provenance, resolved Git commit or archive hash, selected subdirectory, build
  inputs, and installed wheel hashes in the generation record.
- Validate metadata, hashes, wheels, imports, and compatibility before atomically activating a
  generation, normally on restart.
- Do not mutate the live interpreter environment or run `pip install` during add-on activation.
- Prefer pure-Python `py3-none-any` wheels.
- Native wheels must match Python ABI, OS, architecture, Bloom build policy, and any relevant C++ or
  Qt ABI. They are a higher-trust, restart-required capability.
- Reject add-on dependencies that bundle PySide, PyQt, Shiboken, or another Qt runtime into Bloom's
  process.

Embedded trusted add-ons share one resolved environment; dependency conflicts are reported rather
than resolved through unpredictable path ordering. Process-hosted add-ons may use per-add-on
environments and are the long-term default for ordinary third-party code.

## Discovery, Registration, And Lifecycle

```text
installed -> discovered -> validated -> permission-approved
          -> registering -> starting -> running
          -> quiescing -> disabled / faulted
```

Discovery reads package metadata and the Bloom manifest without import. Enablement imports the
entry point only after compatibility and trust decisions.

Add-ons receive one scoped registrar facade backed by approved registries and services. Useful
typed contribution categories include:

- commands/operators, tools, actions, menus, and keymap defaults
- editor descriptions, inspectors, and property schemas
- asset kinds and metadata views
- import/export and standards adapters
- background jobs
- document extension schemas and migrators
- graph node declarations
- render providers through the stricter render contract
- typed event subscriptions

Every contribution has a stable namespaced ID, schema version, capability needs, headless policy,
and owner registration lease. Activation is transactional: duplicate IDs or invalid declarations
fail activation instead of leaving a partially registered add-on.

Registration records cheap descriptors and factories only. File scanning, network access, model
loading, dependency resolution, shader compilation, and other latency-variable work use the task
system.

Disable revokes registrations and subscriptions, rejects new calls, replaces open add-on UI with a
disabled placeholder, cancels owned task groups, suppresses stale results, and stops the runtime
outside the UI thread. Python modules and native extensions are not reliably unloadable; Bloom
reports `Restart required` rather than claiming safe hot unload.

## Events, Tasks, And Diagnostics

Events use typed subscriptions rather than a catch-all bus. Event values contain stable IDs,
revision, sequence, and immutable payloads. High-frequency selection, time, and progress events
support filters, coalescing, bounded queues, and backpressure.

Each subscription and task belongs to an add-on context or script scope and is revoked on disable,
project close, or shutdown. Callback exceptions become per-add-on diagnostics; repeatedly failing
callbacks may be disabled.

The Add-ons surface provides:

- installed/enabled/incompatible/failed/update filters
- publisher, source, license, native-code, platform, and requested-access details
- project-required and missing add-on state
- registered contributions and active jobs
- resolved paths and dependency versions
- startup duration, captured logs, stdout/stderr, and clickable tracebacks
- exportable diagnostic reports

Bloom also provides safe mode, `--disable-addons`, startup recovery for a failing add-on, a package
validator, and exception boundaries. Artists should not need a terminal to understand an add-on
failure.

## Host-rendered UI

The stable public UI tier is schema-driven. An add-on declares property bindings, sections, rows,
lists, trees, actions, validation, diagnostics, task controls, and editor descriptors. Bloom owns
the QWidget tree, so the UI automatically follows:

- Plus Jakarta Sans, Geist Mono roles, and Phosphor icon IDs
- palette, spacing, density, and high-DPI behavior
- keyboard focus, shortcut, and screen-reader semantics
- panel replacement, persistence, missing-editor placeholders, and platform parity
- command, task, and lifetime boundaries

Schema UI serializes across an add-on-process boundary and remains meaningful as metadata in
headless mode even when no widget is created.

## Custom PySide UI

Trusted advanced add-ons may later register complete `QWidget` editor or inspector factories.

- Bloom supplies PySide6/Shiboken built from source against the exact shipped Qt, Python,
  architecture, compiler/runtime, feature configuration, and Release/Debug configuration.
- Add-ons cannot install or bundle another PySide, PyQt, Shiboken, or Qt runtime.
- Keep the non-Qt `_bloom` bridge on pybind11. Generate a separate `_bloom_ui` bridge with Shiboken;
  never bind the same C++/Qt type through both systems.
- Widget construction, destruction, event handlers, and Qt signals run only on the UI thread with
  the correct Python thread state.
- A factory receives a host parent and returns a valid parented QWidget. Bloom owns the area
  lifecycle; wrapper ownership and invalidation use Shiboken rules rather than user-provided pointer
  addresses.
- Callbacks do bounded presentation work and enqueue commands or tasks. They never wait for media,
  I/O, Python futures, processes, GPU work, or render tasks.
- Headless mode reports custom UI contributions as unavailable without disabling the add-on's
  headless-safe services.

Arbitrary Python UI can still freeze the event loop and cannot be preempted safely. This tier is
therefore visibly trusted, GUI-only, exact-release, and normally restart-required. It is not in the
first public add-on SDK.

Qt documents PySide/Shiboken as its official binding stack and recommends matching build
dependencies and Qt versions. See [Qt for Python](https://doc.qt.io/qtforpython-6.8/) and
[building from source](https://doc.qt.io/qtforpython-6/building_from_source/index.html).

## Project Data And Missing Add-ons

Projects record stable add-on/type IDs and schema versions, never Python import paths or pickled
objects. Core owns an opaque extension envelope containing add-on ID, type ID, schema version,
payload encoding, payload, and optional integrity information.

When an add-on is missing, disabled, or incompatible, Bloom:

- preserves its opaque data, nodes, ports, parameters, connections, stable IDs, and layout editor
  IDs without interpretation
- shows a placeholder naming the missing dependency and affected capability
- keeps unrelated project regions editable
- blocks only evaluation/export paths that require the add-on
- offers locate/install-from-file/open-add-ons/continue-degraded actions
- refuses degraded save or produces an explicit loss report if byte-preserving round trip is not
  possible

Re-enabled add-ons propose validated migrations through the document/project boundary. Opening a
project never invokes a migration or embedded script automatically.

## Security And Permissions

In-process Python has the user's process privileges. A manifest's permissions are disclosure,
consent, and least-privilege API shaping—not a sandbox. Bloom must not claim otherwise.

A process boundary gives crash isolation, killable cancellation, and dependency isolation. It is a
security boundary only when Bloom also supplies verified equivalent OS sandboxing on Linux, macOS,
and Windows. Unsupported enforcement is reported as trusted/unsandboxed mode or causes activation
to fail; Bloom never silently weakens the claim.

Scripts embedded in projects remain inert by default. Interactive trust is tied to project identity
and script content hash; code changes invalidate it. Headless execution defaults to deny and
requires an explicit script/add-on allowlist or content hash.

## API Compatibility And Developer Experience

The public SDK requires:

- `bloom.api_version` and declared add-on host API ranges
- capability queries instead of operating-system branching
- stable namespaced command, type, event, editor, and contribution IDs
- independent schema versions for persisted extension records
- deprecation periods and compatibility fixtures
- generated API docs, examples, docstrings, `.pyi` stubs, and `py.typed`
- console autocomplete, `help()`, searchable operation/type references, and runnable templates
- developer-facing UI actions such as Copy Python Path, Copy Python Operation, and Open API Reference
- an exact-runtime developer launcher and disposable development environment
- a package validator and GUI/headless add-on test harness
- API/stub diff checks in CI

The same scripting API runs in an interactive console/editor, explicit startup configuration,
installed add-ons, and `bloom --headless --python script.py`. An externally importable `bloom`
module may follow later and is a separate distribution/ABI commitment.

## Rollout Gates

1. Build and test the language-neutral read, command, transaction, task, event, diagnostics,
   contribution-registration, and opaque-data boundaries.
2. Spike bundled CPython 3.13.x plus pybind11 on Linux, macOS, and Windows using isolated
   initialization, one snapshot read, one command, and one task.
3. Prove a long script cannot block the Qt event loop and shutdown remains bounded.
4. Discover and validate one pure-Python wheel through `bloom.addons.v1`.
5. Prove process-hosted add-on lifecycle, cancellation, diagnostics, schema UI, and missing-addon
   preservation.
6. Publish the first SDK only with docs, stubs, templates, validator, test harness, safe mode, and
   three-platform fixtures.
7. Reconsider exact-version PySide custom widgets and native UI only after lifecycle and latency
   gates pass.

Python render nodes, public native ABI stability, arbitrary PySide widgets, marketplace/update
services, sandbox claims, hot reload, and add-on auto-execution remain deferred.
