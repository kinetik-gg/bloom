# SVT-AV1 4.2.0 security review

Reviewed 2026-10-31. SVT-AV1 consumes only frames Bloom renders itself (it parses no untrusted
bitstream), and it runs only inside the supervised `bloom-media-worker` process, under the worker's
address-space ceiling, per-request deadlines, cancellation and process isolation; an encoder fault
ends that worker and is reported as a typed encode failure. No published advisory affects 4.2.0.

Update policy: track Alliance for Open Media SVT-AV1 releases; replace the archive digest only
through a new intake record and a regenerated lock.
