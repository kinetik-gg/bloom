# OpenH264 2.6.0 security review

Reviewed 2026-10-31. OpenH264 runs only inside the supervised `bloom-media-worker` process, linked
into `libavcodec`, and only its encoder is enabled. The encoder consumes frames Bloom renders
itself, never untrusted bitstreams; the OpenH264 decoder is disabled. The worker's address-space
ceiling, cancellation and process isolation bound a failure to the export attempt.

No open advisory affects the 2.6.0 encoder as configured. Update policy: review Cisco's release
notes and security history with each source update, and replace the archive digest only through a
new intake record and a regenerated lock.
