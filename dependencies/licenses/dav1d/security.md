# dav1d 1.5.4 security review

Reviewed 2026-10-31. dav1d parses untrusted AV1 bitstreams from user media, so it runs only inside
the supervised `bloom-media-worker` process (never in the desktop), under the worker's address-space
ceiling, per-request deadlines, cancellation and process isolation; a decoder fault ends that worker
and is reported as a typed decode failure. dav1d is fuzzed continuously upstream (OSS-Fuzz). No
published advisory affects 1.5.4.

Update policy: track VideoLAN security announcements; replace the archive digest only through a new
intake record, a verified signature and a regenerated lock.
