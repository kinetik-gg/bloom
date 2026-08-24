# Modular Architecture And Pipeline Extensions

Status: accepted direction

Updated: 2026-08-25

## Objective

Bloom is a modular monolith. Its subsystems have explicit ownership and dependency direction, but
they build and run as one cohesive application by default. Modularity exists so substantial
capabilities can be added, replaced, tested, or omitted without forking the product—not so every
class communicates through an abstract interface.

The architecture must leave room for an extreme future extension such as a game-engine pipeline.
Such a pipeline could contribute assets, scene or graph types, evaluators, renderer services,
editor panels, build jobs, and interchange adapters while the ordinary Bloom compositor remains
coherent and independently buildable.

## Design Balance

Bloom deliberately avoids both extremes:

```text
Tightly coupled application        Bloom                      Plug-in microkernel
---------------------------   ------------------   ----------------------------------
Subsystem internals leak          Modular monolith       Every call behind an interface
Features require core edits       Typed module APIs      Generic service locator everywhere
No replaceable pipelines          Explicit registries    Runtime discovery for ordinary code
                                  Static by default       Fragile binary ABI as the foundation
```

Within a module, use normal C++ values, concrete types, direct function calls, and ordinary
composition. Introduce an interface only at a boundary with an actual reason: multiple
implementations, optional ownership, platform isolation, asynchronous execution, external
integration, or test substitution.

## Module Classes

### Foundation Modules

The document, commands, task system, project I/O, runtime, render contracts, media services,
platform services, and UI shell are first-party foundation modules. They are compiled together and
may expose typed C++ APIs to declared dependants. They do not need a stable binary ABI.

### Optional Pipeline Modules

An optional pipeline module is a first-party or trusted source-built feature bundle. It may be
enabled by the build or application configuration and contributes capabilities through the host
registries. Examples include an editorial pipeline, grading pipeline, or a future game-engine
pipeline.

Optional modules are statically linked by default. Static linking keeps deployment, optimization,
debugging, Qt compatibility, and lifetime behavior straightforward while preserving architectural
replaceability.

### External Native Modules

External binary modules are a future distribution feature, not the initial architecture. A public
binary boundary must not expose the compiler-dependent C++ standard-library ABI, Qt objects, or
internal document types. When this enters scope, use a versioned C ABI, generated bindings, or a
process boundary with explicit data contracts.

A native editor-panel extension that directly uses Qt may require the exact Bloom SDK, compiler,
Qt minor version, and platform ABI. It is a separate compatibility tier from a stable data or
service extension API and must be labeled honestly.

### Isolated Modules

Untrusted, crash-prone, or independently versioned integrations may run out of process. Isolation
is selected for a concrete safety or compatibility need; it is not the default cost paid by every
in-tree feature.

## Composition Root

`apps/bloom` is the composition root. It selects modules, constructs long-lived application
services, resolves declared dependencies, and starts or stops modules. Product code must not grow a
second hidden composition root.

```text
apps/bloom
  |
  +-- Module Catalog and dependency validation
  +-- Host Services
  |     +-- command dispatcher
  |     +-- task scheduler
  |     +-- document/project services
  |     +-- runtime/render/media/platform services
  |     +-- diagnostics and capability reporting
  |
  +-- Registries
        +-- editor panels and actions
        +-- asset and document extension types
        +-- commands and tools
        +-- node definitions and evaluators
        +-- render backends
        +-- import, export, and standards adapters
```

Host services are passed explicitly to module construction or startup. Bloom does not use a
process-global service locator that lets any code reach every service. Registries are used for
extensible vocabularies; they do not replace direct typed dependencies between known foundation
modules.

## Module Contract

Every module declares a descriptor containing at least:

- stable module ID and human-readable name
- module version and required host API range
- required and optional module dependencies
- capabilities it provides and consumes
- whether it is built in, trusted native, or isolated
- platform, architecture, GPU, and third-party dependency requirements

The conceptual lifecycle is:

```text
discover -> validate descriptor -> resolve dependency DAG -> register declarations
         -> construct/start services -> run -> stop services -> unregister
```

The initial implementation only needs compiled-in discovery and startup. Runtime installation,
hot reload, and unloading are not required. In particular, module unloading must not be promised
until document objects, tasks, GPU resources, callbacks, and Qt panels can prove safe lifetime
behavior.

Module dependencies form a directed acyclic graph. Cycles indicate that responsibilities or shared
contracts need to move into a lower-level owning module. Optional collaboration between peers uses
a narrow shared contract or registry rather than undeclared back-pointers.

## Extension Surfaces

A pipeline module may register coherent contributions in these areas:

| Surface | Contribution | Boundary rule |
| --- | --- | --- |
| Editor | panel types, tools, actions, inspectors | uses shared context and commands; no raw document mutation |
| Assets | asset kinds, metadata views, import/probe services | stable IDs and asynchronous media/task services |
| Document | namespaced extension records and validators | versioned, serializable, preservable when unavailable |
| Commands | typed operations and undoable transactions | same validation, dirty-state, and event path as built-in edits |
| Graph/runtime | node definitions, compilers, evaluators | explicit time, color, alpha, cache, capability, and fallback contracts |
| Rendering | render backend or specialized execution provider | immutable requests, parity policy, device-loss handling |
| Pipeline I/O | standards adapters, build/package/export steps | preservation/loss reports and no silent degradation |
| Jobs | analysis, conversion, build, and background services | task ownership, progress, cancellation, and diagnostics |

Registration adds capabilities to an owning registry. It does not grant direct access to private
state in another module.

## Document And Project Compatibility

Module-provided authoring data uses stable, namespaced type IDs and schema versions. The project
format records required module IDs and relevant capability versions.

When a required module is unavailable, Bloom should where safe:

- preserve its namespaced authoring data without interpreting or rewriting it
- show explicit missing-capability placeholders and dependency diagnostics
- keep unrelated parts of the project editable
- block only evaluation or export paths that genuinely depend on the module
- avoid executing module or project code merely by opening a project

Saving from a degraded session must not silently discard unknown module data. Any unavoidable loss
requires an explicit report and confirmation under the standards and project-format policies.

## Pipeline Example: Game Engine

A future `kinetik.game-engine` pipeline could be one optional module bundle that registers:

- engine project and scene asset types
- importers for engine-native assets and open scene standards
- scene, material, animation, profiler, and build editor panels
- graph nodes that consume or produce Bloom image and data resources
- a real-time renderer or specialized render backend
- asynchronous shader compilation, asset cooking, packaging, and live-link jobs
- export and synchronization commands

Bloom would provide the workspace shell, selection/context model, command and undo system, task
system, diagnostics, project persistence boundaries, and shared media/color services. The game
engine module would own its domain model and pipeline behavior. Neither side would need to pretend
that game-engine scenes are native composition layers.

This example is an architecture fitness test, not committed product scope.

## Threading And Resource Rules

Modules obey the same non-blocking contract as built-in features:

- registration records cheap declarations and factories; it does not scan assets or compile work
- startup schedules latency-variable initialization through the task system
- module tasks are scoped so disable, project close, and shutdown can cancel them
- callbacks publish through host-owned lifetime tokens and reject stale results
- GPU work uses render-service contracts rather than creating hidden UI-owned devices
- a module cannot wait for tasks, GPU fences, processes, or I/O on the Qt UI thread

## API And Compatibility Policy

There are separate compatibility promises:

1. In-tree source API: evolves with Bloom and is updated atomically in the repository.
2. Trusted module SDK: versioned and supported for a declared Bloom release range when introduced.
3. Stable external ABI or protocol: introduced only with compatibility fixtures and lifecycle rules.
4. Project data compatibility: namespaced schemas remain preservable even when executable support
   is absent.

Do not claim a stable plug-in SDK merely because internal interfaces exist. The host API version,
module descriptor, capability negotiation, and compatibility test kit must exist before that claim.

## Dependency And Design Rules

- Each module has one documented owner and a small public surface.
- Modules expose behavior and domain values, not internal containers or mutable stores.
- Depend on the smallest owning module rather than on `apps/bloom` or the entire UI.
- Keep Qt out of non-UI module contracts and backend-specific types out of product contracts.
- Do not use a catch-all event bus for commands, document changes, tasks, or render requests.
- Prefer typed synchronous calls for bounded work and the task system for latency-variable work.
- Do not add an abstraction solely to make a diagram look decoupled.
- Do not let convenience imports create circular build dependencies.
- Make optional dependencies explicit and capability-tested.

## Testing

The module architecture needs tests for:

- deterministic dependency resolution and cycle/missing-dependency diagnostics
- duplicate IDs, incompatible versions, and conflicting registrations
- identical behavior between built-in and registered implementations where they share a contract
- project open/save with required, missing, and upgraded module schemas
- task cancellation and stale callbacks during project close and shutdown
- panel replacement and workspace restoration with module-provided editor types
- render and standards conformance for module-provided nodes and adapters
- Linux, macOS, and Windows capability and packaging behavior

## Initial Implementation Boundary

The first vertical proof needs only a small compiled-in module catalog plus typed registries for
editor panels and node definitions. The document, command, task, and render modules may otherwise
use direct C++ APIs.

Add the shared module descriptor and host-service boundary when the second independently owned
pipeline capability needs registration. Dynamic loading, third-party installation, hot reload, and
process isolation remain deferred until there is a concrete workflow and compatibility policy.
