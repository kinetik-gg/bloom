# FFmpeg 8.1.2 security review

Reviewed 2026-09-17 against the official FFmpeg security history at
<https://ffmpeg.org/security.html>. The 8.1.2 point release records fixes for CVE-2026-8461 and
CVE-2026-30999. Those release fixes are the disposition for the vulnerabilities listed in the
lock; no separate local patches are applied.

FFmpeg parses untrusted media and therefore remains worker-only. The future worker must enforce
bounded input, memory, CPU, output, and wall-clock limits; cancellation and crash containment are
part of the provider contract. The desktop process must never load the FFmpeg shared libraries.

Update policy: monitor the upstream release and security pages for the selected maintenance line,
review every relevant CVE against the enabled component set, and refresh the archive, signature,
license, provenance, review, security, lock, and qualified-prefix evidence together. A security
update is not accepted by changing only the version string or by silently widening this allow-list.
