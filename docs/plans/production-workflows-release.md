# Production workflows release

Status: landed on main; open work tracked in issues

Updated: 2026-10-25

Parent: [#374](https://github.com/kinetik-gg/bloom/issues/374)

## Scope

This release makes the timeline, footage, masking and EXR workflows usable in production on Linux.
macOS and Windows provider work stays tracked in [#370](https://github.com/kinetik-gg/bloom/issues/370)
and [#371](https://github.com/kinetik-gg/bloom/issues/371) with their implementation paths. The owning
architecture, UX and decision documents remain authoritative; this record only lists what landed and
what is open.

## Landed

| Area | Issue | Result |
| --- | --- | --- |
| Timeline | [#353](https://github.com/kinetik-gg/bloom/issues/353) | Layer-owned time (ADR 0024): start, slip, speed, trim, Before/After hold, loop and ping-pong, Time Remap; Timeline move, Alt slip/stretch, trims; Properties Time section; schema 1.23; plan semantics 9 |
| Timeline | [#354](https://github.com/kinetik-gg/bloom/issues/354) | Audio follows layer in-points, speed, layer and video loop modes, and value-driven Time Remap |
| Performance | [#355](https://github.com/kinetik-gg/bloom/issues/355) | Proven render footprints: unreachable node edits and parameter/key edits no longer discard the whole RAM preview |
| Performance | [#356](https://github.com/kinetik-gg/bloom/issues/356) | Identity-verified video sources (ADR 0025): no full re-read or re-hash per edit, tick, export frame or audio block |
| Performance | [#357](https://github.com/kinetik-gg/bloom/issues/357) | Incremental commit validation and copy; operation warnings surfaced |
| Performance | [#358](https://github.com/kinetik-gg/bloom/issues/358) | Playback repaints only playhead strips, caches key summaries and driver probes, remixes audio only on edits that can change it |
| Masking | [#360](https://github.com/kinetik-gg/bloom/issues/360) | Layer masks, Mask values, track mattes (ADR 0028), animated paths as GPU data, live drag preview, shared path editing with Corner/Mirrored/Aligned/Free/Auto handle modes, scripting path values |
| Masking | [#361](https://github.com/kinetik-gg/bloom/issues/361) | Set Alpha / Multiply Alpha: absent matte on every route, invert, matte selector |
| EXR | [#362](https://github.com/kinetik-gg/bloom/issues/362) | Alpha association inherited from the asset, non-finite repair before colour conversion, data windows kept registered |
| EXR | [#363](https://github.com/kinetik-gg/bloom/issues/363) | Multi-channel images (ADR 0026): parts and layers, ChannelSet transport, Shuffle, Channel Copy, Select Channels, Viewer channel inspection, layered EXR output; schema 1.26 |
| EXR | [#364](https://github.com/kinetik-gg/bloom/issues/364) | Cryptomatte node with Mask and Image outputs, manifest names, ID-colour preview, Viewer eyedropper through transforms and time |
| Video | [#365](https://github.com/kinetik-gg/bloom/issues/365) | Untagged sources assume Rec.709 (HD) or Rec.601 (SD) with a visible note; BT.2020, PQ and HLG; exact PQ LUT sampling; chroma siting |
| Video | [#366](https://github.com/kinetik-gg/bloom/issues/366) | FFmpeg SIMD, threading, forward decode, VA-API preview decode (ADR 0027), cache sizing |
| Video | [#367](https://github.com/kinetik-gg/bloom/issues/367) | VFR slots, orientation, pixel aspect, open-GOP seek, bwdif deinterlacing, RGB-coded streams, audio stream selection, field order and timecode metadata (schema 1.25) |
| Video | [#352](https://github.com/kinetik-gg/bloom/issues/352) | 8K and long-source limits, per-stream worker address space, lazy container index, snapshot quota from free space, status-bar video cell |
| Import | [#373](https://github.com/kinetik-gg/bloom/issues/373) | Batch import keeps every valid file and reports each skipped one |
| Tests | [#375](https://github.com/kinetik-gg/bloom/issues/375) | GPU media and export tests that failed on main repaired at their causes |
| Composition | [#376](https://github.com/kinetik-gg/bloom/issues/376) | One New Composition / Composition Settings dialog with presets, user presets, exact frame rates, drop-frame timecode, resize anchor and safe areas |
| Assets | [#378](https://github.com/kinetik-gg/bloom/issues/378) | Project-panel style Assets: folders (schema 1.27), sortable columns, selection header, context menus, usage counts, import into the selected folder |

Also landed: Viewer tools return to Select after creation and Escape confirms every Viewer edit;
concurrent GPU preparations share device memory instead of the first taking all of it; one GPU device
per Python interpreter with serialized teardown; a Transform node category.

## Open

| Issue | Work |
| --- | --- |
| [#377](https://github.com/kinetik-gg/bloom/issues/377) | Blocking, cancellable import progress dialog |
| [#369](https://github.com/kinetik-gg/bloom/issues/369) | Export verified end to end on every route, then colour tags, MP4/H.264, HEVC, DPX, threading and size limits |
| [#368](https://github.com/kinetik-gg/bloom/issues/368) | Codec coverage expansion |
| [#372](https://github.com/kinetik-gg/bloom/issues/372) | Proxy generation and proxy-aware playback |
| [#359](https://github.com/kinetik-gg/bloom/issues/359) | Nodes canvas rebuild and lookup costs |
| [#370](https://github.com/kinetik-gg/bloom/issues/370), [#371](https://github.com/kinetik-gg/bloom/issues/371) | Windows Media Foundation and macOS VideoToolbox providers |

Known validation gaps at landing: `bloom.quality.architecture_boundaries` and `bloom.quality.ui_grammar`
report existing findings, `bloom.ui.live-value-edit` can abort under parallel load (passes alone), and
the GPU executor's per-image ceiling refuses 8K float frames when the operation-cache budget is below
about 2 GiB. These are tracked under #374.

## Validation at landing

- Full Linux desktop build with warnings as errors.
- Full CTest: 465 of 469 pass; the four exceptions are the gaps above plus the native acceptance gate,
  which requires a Wayland platform.
- GPU route proofs under a private headless compositor: viewer, RAM preview, still, sequence/range,
  video and headless routes all pass; native coverage 98 pass, 0 fail, 0 missing.
