# ADR 0032: Ordinary SDR Video Is Shown As Players Show It

Status: accepted (issue #381, step 1)

Date: 2026-10-30

## Context

Ordinary Rec.709 and untagged SDR video (phone and camera H.264) looked washed out by default and
only looked right after its input colour was changed. The cause was in Auto resolution, not in the
YUV conversion:

- A config-managed video conversion decodes only the stream's Y'CbCr matrix and range and leaves
  the transfer to the input colour space (`videoToInputColorSpace`, transfer code 8).
- Under Bloom Neutral v1, Auto resolved Rec.709 and assumed Rec.709/601 video to the config's
  "Rec.709 video space", which Neutral mapped to its scene-linear `lin_rec709_scene`. The encoded
  R'G'B' therefore entered the working space as linear light, and the default sRGB display view
  encoded it a second time. Measured on a 1280x720 libx264 SMPTE-bars clip, the 75% white bar that
  FFmpeg's player-equivalent decode shows as code 191 rendered as 224 (sRGB-encoding 191/255 again),
  with a mean error of 26 codes over the frame, identically for the tagged and the untagged clip.
- Under the ACES 1.3 CG config, Auto used the synthesized `Rec.709 Camera (Bloom)` space (the
  BT.709 inverse OETF), a camera interpretation that also differs from what players and Bloom's
  own sRGB stills show.

The existing tests compared Auto with an explicit choice of the same space, so they could not see
the double encoding.

## Decision

1. **Auto shows ordinary SDR video through the sRGB texture space.** A stream whose tags (with
   missing tags filled by the existing size convention: Rec.709 for HD, Rec.601 for SD, limited
   range) are a Rec.709 or Rec.601 matrix with the BT.709/SMPTE 170M transfer resolves to the
   config's sRGB texture space, the space Bloom's 8/16-bit PNG, JPEG and TIFF stills use
   (`srgb_rec709_display` in Bloom Neutral, `sRGB - Texture` in ACES 1.3 CG). The YUV step still
   decodes the stream's matrix and range; the resulting R'G'B' is interpreted with the sRGB
   display transfer, as players show it. Through the default sRGB view, footage matches players
   and matches Bloom's sRGB stills without adjustment. An sRGB-tagged stream already resolved there.
2. **Camera interpretations stay explicit.** `Rec.709 Camera (Bloom)` (BT.709 inverse OETF) is
   synthesized into every pinned config that lacks a Rec.709 camera space, now including Bloom
   Neutral (no primaries matrix there, since Neutral's reference is linear Rec.709), so the camera
   interpretation is an explicit Input Colour choice under every pinned config. PQ, HLG, Rec.2020,
   log and every other explicit or legacy choice are unchanged, and HDR Auto resolution is
   unchanged.
3. **The visible note says what is assumed.** An untagged SDR stream's note reads "Colour tags
   missing — assumed Rec.709 with the sRGB transfer. Change in Assets → Input Colour" (Rec.601 for
   SD). The assumption identity becomes `assumed-colour-v2`, and the resolved input space id is part
   of video cache identity, so every cached frame, thumbnail and RAM-preview entry made under the old
   default is a different key.
4. **Older projects get a one-time note.** Document 1.28 changes no member; the 1.27 → 1.28 step is
   version-only. Opening a project saved before 1.28 that has a video asset in Auto whose stored tags
   take this default shows an information note on the existing Open Project surface naming how many
   video assets now look less pale and where to change them. Saving records 1.28, so the note does
   not return. The assumption is not recorded per asset: the old look was a defect, not a choice,
   and explicit choices (including an explicit `lin_rec709_scene`) were always stored and are kept
   exactly.
5. **Regression evidence.** Generated 64x48 H.264 colour-patch fixtures (BT.709-tagged, and
   untagged SD so Rec.601 is assumed) carry FFmpeg/swscale's RGB24 decode of their first frame.
   Both the CPU reference route and the GPU route (the sRGB-texture to working-space conversion is
   a dispatched GPU OCIO effect with CPU parity, read back once) are shown through the default view
   and must match it within 2 codes per channel and a mean of 0.75 code over patch interiors.

## Consequences

- The default look of every Auto SDR video changes, in new and old projects alike. Under ACES 1.3
  CG the default ACES view still tone-maps, so the frame matches Bloom's sRGB stills in that config
  rather than a player.
- Users who want the camera reading (scene-linear values that follow the BT.709 OETF) choose
  `Rec.709 Camera (Bloom)` explicitly. The previous Neutral look can be reproduced exactly by
  choosing Linear Rec.709 (`lin_rec709_scene`), though it was never intended.
- Under Bloom Neutral the default video path now carries a real (non-identity) input transform, so
  the GPU route runs one OCIO effect for SDR video where it previously had none.
