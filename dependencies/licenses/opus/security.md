# Opus 1.5.2 security review

Reviewed 2026-10-31. Bloom builds only the libopus encoder path it calls (the library also contains
a decoder FFmpeg does not use here; FFmpeg's native decoder reads Opus). libopus consumes PCM that
Bloom mixes itself and runs only inside the supervised `bloom-media-worker` process, under the
worker's address-space ceiling, per-request deadlines, cancellation and process isolation. libopus
is fuzzed continuously upstream (OSS-Fuzz). No published advisory affects 1.5.2.

Update policy: track Xiph security announcements; replace the archive digest only through a new
intake record and a regenerated lock.
