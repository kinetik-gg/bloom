# nv-codec-headers 12.1.14.1 security review

Reviewed 2026-10-31. The component is declarations only; it parses nothing and contains no
executable code of its own beyond FFmpeg's dynamic loader, which resolves the NVIDIA driver library
by its system name. NVENC runs inside the supervised `bloom-media-worker` process, encoding frames
Bloom renders itself; a driver fault ends that worker and the encode chain falls back to the next
provider. No published advisory affects these headers.

Update policy: move to a newer header line only together with a decision on the minimum supported
driver, through a new intake record and a regenerated lock.
