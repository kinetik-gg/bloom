# miniaudio 0.11.25 Security Review

Reviewed: 2026-09-15

## Disposition

The device backend does not parse user media. It receives only Bloom-owned rate, channel, and
callback configuration and writes already-decoded samples to the selected platform device. The
backend therefore is not part of the file-parser trust boundary, but miniaudio still executes
in-process and remains an accepted v0 dependency risk.

The backend never opens a project path, owns no media buffers, and exposes no miniaudio type through
a Bloom header. Initialization failure is reported as a backend error; the engine remains usable
with `NullBackend` for headless tests. The callback performs only bounded ring-buffer reads and
zero-filling, so it does not allocate, lock, decode, or perform file I/O.

Re-review is required if miniaudio is used for decoding, if device callbacks gain blocking or
allocation behavior, if the backend becomes a project-data authority, or if an upstream security
advisory affects the pinned commit.
