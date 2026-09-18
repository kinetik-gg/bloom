# OpenH264 2.6.0 security review

Reviewed 2026-09-18. The source library is worker-only and is used solely to build FFmpeg's
link-time wrapper dependency. It is not loaded by Bloom's desktop process and is not loaded from
the build prefix at runtime.

The Cisco binary is treated as an untrusted externally fetched executable dependency. Every launch
requires an exact SHA-256 match against the reviewed decompressed digest. Downloads are made by a
supervised `curl` process with an HTTPS URL, a 60-second deadline, a 16 MiB cap, staging-file
publication, and a second digest check. Truncated, over-limit, mismatched, or partially published
files are refused. A user-located file follows the same digest check.

Update policy: review Cisco's release notes, binary licence, source release, and security history
together. Never replace the runtime digest or URL without a new intake record and generated lock.
