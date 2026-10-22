# User control and policy simplification

Status: active implementation and source audit; runtime qualification is recorded separately

Updated: 2026-10-22

Owner: [issue #320](https://github.com/kinetik-gg/bloom/issues/320).
Source baseline: `45431368`; current corrections are not claimed as passed until their grouped local gates finish.

## Decision

A contract protects document meaning, ownership, cancellation, valid resource use and reliable publication.
It must not silently substitute a smaller user request or make a cache preference control whether a frame
can render. Prefer one authoritative admission boundary over overlapping arbitrary limits. Keep real
schema, overflow, codec/device, allocation and retirement checks. Removing a check is not sufficient
when the representation still eagerly allocates an entire file or every missing sequence frame.

## Bounded implementation order

### 1. [#321: Honor live cache preferences and independent GPU working capacity](https://github.com/kinetik-gg/bloom/issues/321)

Parent: #320.

Remove host reserve/percentage trim/recovery ladders, proportional rewriting of explicit operation/RAM targets, and decoded-video deduction. Apply memory preferences immediately. Automatic targets resolve once from physical RAM. Only an observed finite absolute OS limit may constrain combined retention, with honest UI. Foreground host/device work must not depend on retention targets or permanently freeze startup free space.

Scope: memory ledger, delivered phase view, cache preference wiring, GPU owner capacity, status/prefs. Preserve held claims, zero/unknown distinction, fresh constraint removal, native capability checks, cancellation and retirement. Test explicit targets above automatic defaults, auxiliary independence, live Apply, low cache with renderable frame, transient device-zero recovery, finite OS constraints, and Full/Half warm preview. One grouped local build/strict/CPU fallback/native-route milestone; no Actions.

### 2. [#322: Preserve exact numeric values and untouched components across editors](https://github.com/kinetik-gg/bloom/issues/322)

Parent: #320.

Node cards clamp valid values to ±1e9 and commit entire vectors; Properties/Timeline impose different caps. Editing Y can overwrite an untouched X. Replace arbitrary ranges with schema validity, use exact int64 parsing, component commands and bounded scientific display. Keep value/unit/conversion factor stable during a gesture. Match grid size and Automatic column-width preferences to runtime behavior.

Scope: value field, node card, Properties, Timeline, focused UI regressions. Verify large finite values, int64 extrema/overflow, untouched siblings, cross-surface round trip, undo/cancel, and percentage/native-unit editing. Semantic opacity and positive-only domains remain. Validate with the first grouped local milestone.

### 3. [#323: Preserve color intent in Project Settings, pickers and headless renders](https://github.com/kinetik-gg/bloom/issues/323)

Parent: #320.

Project Settings resets existing ACES working spaces or external configs on untouched OK; composition color converters stay stale after override/undo; scripting still/range hardcodes Rec.709 and MCP media export defaults to Neutral.

Implement sequential slices: (1) exact unchanged-settings preservation and effective-color-identity picker invalidation; (2) shared captured project/composition intent through Python/CLI/MCP render. Preserve external settings without pretending external OCIO runtime support exists. Tests: nondefault ACES and unknown/external no-op OK, explicit config edits, override/undo swatches, same saved composition desktop/headless color identity and pixels. Keep preparation off UI. No unrelated effect expansion.

### 4. [#324: Remove repeated full-project validation from node search and undo](https://github.com/kinetik-gg/bloom/issues/324)

Parent: #320.

Opening Add search constructs and validates a Document for each candidate (up to111 current entries). Undo/redo revalidates privately minted immutable snapshots already validated at publication.

Replace trial execution in search/menus with shared read-only eligibility over one immutable snapshot; retain command-time validation. Remove only restore's redundant scan, retaining document provenance, revision, allocator high-water and publication guards. Owning files: node_editor_menus/node_editor_add, node_operations, document restore. Work-counter regressions must prove no per-entry validation and eligibility matches real commands; preserve undo/redo integrity. Group local checks with the color UI slice.

### 5. [#325: Make input interpretation, thumbnails and export color choices coherent](https://github.com/kinetik-gg/bloom/issues/325)

Parent: #320. Depends on color-intent correctness slice.

Add an undoable asset-level input interpretation control. Show effective precedence between source legacy Color Space and config Input colour space so a visible choice cannot silently do nothing. Prepare still/video thumbnails using a qualified display processor and cache its identity. Preflight named ACES handoff/review choices against the selected config before export.

Keep this as three sequential bounded slices: asset command+picker; thumbnail display processing; output capability-aware dialog. Tests cover asset/source override/invalidation, ACES thumbnail vs display output, unsupported presets and explicit diagnostics. External OCIO runtime support is separately planned, not enabled by selectors alone.

### 6. [#326: Replace silent text and composition limits with checked requested semantics](https://github.com/kinetik-gg/bloom/issues/326)

Parent: #320.

Text Repeat silently caps1024, Pad256, and decimal formatting17 fractional places even when output fits. Composition dialog rounds rates to millifps and a million-frame widget cap can change a valid duration on a rate edit.

Two bounded slices: (1) honor text count/width/precision within checked output byte/cancellation bounds; report budget exhaustion instead of success with altered output. (2) exact rational frame-rate entry/presets and canonical duration/frame-count handling constrained by representability. Test small outputs beyond old caps, empty/multibyte cases, exact30000/1001, long duration unit/rate changes, overflow and undo. Do not remove real allocation or arithmetic checks.

### 7. [#327: Remove overlapping MCP restrictions and make large queries pageable](https://github.com/kinetik-gg/bloom/issues/327)

Parent: #320.

The JSON parser's4096-element array cap rejects atomic transactions already fitting request bytes, total values, depth and allocation bounds. Parameter queries emit every full string, duplicate structured output as text, then hit4MiB response cap.

Remove redundant per-array rejection while preserving existing total/depth/byte accounting and transaction atomicity. Add bounded deterministic query paging/individual-value retrieval and avoid unnecessary response duplication where protocol compatibility permits. Test a compact5000-command transaction, malformed/deep/oversized input rejection, large parameter data across pages and complete untruncated results.

### 8. [#328: Scope purge coordination to cache consumers and simplify trusted project reads](https://github.com/kinetik-gg/bloom/issues/328)

Parent: #320.

Purge currently waits for global scheduler quiescence and fails after5s if unrelated Save/I/O is active. ZIP ratio1000:1 duplicates admitted expanded-size checks and forces highly compressible JSON to be stored uncompressed.

Separate slices: purge waits for relevant producer tasks/leases while preserving producer gating and late-publication exclusion; ZIP removes only ratio-based rejection/fallback with deliberate old-reader compatibility, preserving compressed/expanded limits, charged allocation, exact-size extraction, CRC and atomic publication. Test held unrelated Save vs held cache consumer, cancellation/retirement; ZIP high-compression roundtrip and actual expansion/CRC bounds. Never delete the purge barrier wholesale.

### 9. [#329: Honor external media locations and budget actual import/decode work](https://github.com/kinetik-gg/bloom/issues/329)

Parent: #320.

Verified restrictions: cross-drive Windows locator requires lexical relative path; audio probe reads whole file and defaults64MiB, decode48M interleaved samples, while useful errors are hidden; sequence scanner counts unrelated directory entries against100k and materializes every gap.

Implement bounded sequential slices: (1) explicit external absolute file-URI locator fallback with save/relink/digest behavior; (2) typed audio diagnostics then streaming probe/windowed admitted decode; (3) cancellable matching-only sequence scan and interval gaps. Preserve actual byte/metadata admission and missing-frame semantics. Tests: drive/UNC roots, long valid audio beyond old thresholds, unrelated files, sparse/long sequences, cancellation and recovery. Removing caps without fixing whole-file/gap allocations does not complete this issue.

### 10. [#330: Unify final-render admission with actual capacity instead of fixed fractions](https://github.com/kinetik-gg/bloom/issues/330)

Parent: #320; coordinate #304 allocation ownership.

Final render has a separate availableRAM/4 allowance and synchronous OS sampling; output analysis imposes fixed dimension/pixel/per-job/concurrent limits independently of actual hardware. These can refuse valid work despite corrected preview preferences.

First replace repeated synchronous sampling/default fractions with one captured off-UI capacity/admission view for still/range/video/headless. Then qualify configurable/capability-based output bounds across analysis/adapters/reservation identity; preserve representability, codec/device limits, exact peaks, cancellation, one final readback, and atomic verified publication. Tests must cover low retention with valid render, actual capacity refusal/recovery, large output analysis, all production routes. Do not use unbounded numeric maxima as invented capacity.

## Additional audit candidates requiring confirmation

- History retention can reject an otherwise valid command when a before/after snapshot charge exceeds
  the fixed 512 MiB budget (`src/commands/command_stack.cpp`). Determine a concrete large-document
  trigger and distinguish retained history cost from executing the edit before changing admission.
- Recovery discovery compares candidates against the active project's modification time and returns
  only the newest candidate (`src/host/session_recovery.cpp`). Verify project identity and candidate
  selection before claiming recovery correctness; do not delete or overwrite recovery files.

## Validation and supervision

Sol high/xhigh workers own bounded files and acceptance cases. Independent review is required before
closing the integrated milestone. Use local runners and existing qualified build directories, with
one build/regression/strict/native group per milestone; rerun only failures or affected code after a
repair. Keep source identities and native NotRun/Skipped results honest. Never add GPU coverage opt-outs.

The user's current application and primary checkout edits remain separate from validation fixtures.
Issue #304 still owns broader physical allocation accounting and cross-platform native qualification.
The closed earlier sprint #306 does not certify these newly discovered problems.

