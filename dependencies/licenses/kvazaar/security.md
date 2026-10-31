# Kvazaar 2.3.2 security review

Reviewed 2026-10-31. Kvazaar is an encoder that runs only inside the supervised
`bloom-media-worker` process, linked into `libavcodec`. It consumes frames Bloom renders itself and
parses no untrusted bitstreams. The worker's address-space ceiling, cancellation and process
isolation bound a failure to the export attempt. No published advisory affects 2.3.2.

Update policy: review upstream release notes with each update and replace the archive digest only
through a new intake record and a regenerated lock.
