# libvpx 1.17.0 security review

Reviewed 2026-10-31. Bloom builds only libvpx's VP9 encoder, which consumes frames Bloom renders
itself; the VP9 decoder (the component with the published decoder advisories) is not compiled.
The encoder runs only inside the supervised `bloom-media-worker` process, under the worker's
address-space ceiling, per-request deadlines, cancellation and process isolation; an encoder fault
ends that worker and is reported as a typed encode failure. No published advisory affects the
1.17.0 VP9 encoder.

Update policy: track WebM Project libvpx releases and Chromium security notes; replace the archive
digest only through a new intake record and a regenerated lock.
