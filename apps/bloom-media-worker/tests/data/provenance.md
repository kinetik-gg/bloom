# VP9 and AV1 decode reference fixtures

Three tiny, original synthetic clips that exercise Bloom's bundled VP9 (FFmpeg's native decoder)
and AV1 (dav1d) decode paths with streams from independent reference encoders. Bloom's own build
has no VP9 or AV1 encoder at this intake, so these bytes cannot be generated at test time; they are
small enough to review byte for byte and contain no third-party content.

Content: a flat Rec.709 limited-range colour (lavfi `color=c=0x3366cc`), 64x48, 12 frames at 24
fps, tagged BT.709 primaries, transfer and matrix with limited range. Every decoded sample has one
exact code: 8-bit Y'CbCr 98/177/101, 10-bit 403/714/407.

| File | Codec | Encoder | SHA-256 |
| --- | --- | --- | --- |
| `vp9-flat.webm` | VP9 Profile 0, 4:2:0 8-bit, WebM | libvpx 1.17.0 | `c3236e990f80052df550dbac647103f84772e1dc564d562ca7b8a72177b27d8e` |
| `av1-flat.mp4` | AV1 Main, 4:2:0 8-bit, MP4 | libaom 3.15.1 | `aa0b24bc7eacf8398b5718024f749f3fed4ec14ea349131158a06285d16b40f8` |
| `av1-flat10.webm` | AV1 Main, 4:2:0 10-bit, WebM | libaom 3.15.1 | `a90e41e10eef04e3daf601d147913a1493f905f405a9d983518835ef9177ba0b` |

Generated 2026-10-31 with FFmpeg 9.0.2 (bit-exact flags), for example:

```sh
ffmpeg -f lavfi -i "color=c=0x3366cc:s=64x48:r=24:d=0.5,format=yuv420p" \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv \
  -c:v libvpx-vp9 -b:v 0 -crf 10 -g 12 -row-mt 0 -threads 1 -bitexact -fflags +bitexact \
  vp9-flat.webm
```

The AV1 files use `-c:v libaom-av1 -crf 10 -g 12 -cpu-used 8 -threads 1` with the same input
(`format=yuv420p10le` for the 10-bit file).
