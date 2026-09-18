# COLOR-5 identity oracle

This standalone calculation imports no Bloom code. Run the Python block below to reproduce the
neutral display/media digest pins and the ACES handoff record lengths. The C++ tests pin these
results; the existing default EXR golden records remain unchanged.

```python
"""Independent byte oracle for COLOR-5's default display and media analysis records."""
import hashlib
import json
import struct


def text(value):
    data = value.encode("utf-8")
    return struct.pack(">I", len(data)) + data


revision = "a6fc07c95cea14897b725992a179d35055cccce2ffaf719ca73b4a289c89eacf"
display_record = (
    b"BloomDisplayProcessorIdentity\0"
    + struct.pack(">H", 1)
    + bytes.fromhex(revision)
    + struct.pack(">H", 0)
    + text("lin_rec709_scene")
    + text("srgb_rec709_display")
    + text("srgb_rec709_display")
    + b"\0\0\0"
    + text("srgb_rec709_display")
    + text("reference")
    + text("bloom.color.ocio-cpu-display.v1")
    + text("straight-rgba8")
)
display_digest = hashlib.sha256(display_record).hexdigest()
prores_note = "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation"
facets = [
    (1, 3, "OCIO display/view; clamp; lossy video"),
    (2, 3, "Float32 to RGBA16 and codec sample precision"),
    (3, 3, "display: srgb_rec709_display; view: srgb_rec709_display; OCIO: " + revision),
    (4, 4, "Alpha omitted"),
    (5, 2, "RGB to codec YUV; PCM channel layout retained"),
    (6, 1, "Origin at zero required"),
    (7, 1, "Equal data and display windows required"),
    (8, 1, "Square pixels required"),
    (9, 3, "Intra-frame lossy codec; immutable tolerance profile"),
    (10, 2, "Exact rational cadence, duration, stream layout; optional BWF description; look: baked (0 look-tagged effects)"),
    (11, 5, prores_note + "; display identity=" + display_digest),
]
analysis_record = (
    b"BloomMediaOutputAnalysisV1\x04"
    + bytes.fromhex("3eb779c366d82c3f12d9fabeb1dce7deff69e740e7db866fe619db7a4d804da2")
    + b"".join(bytes((facet, state)) + description.encode("utf-8") + b"\0" for facet, state, description in facets)
)
records = {
    "display_bytes": len(display_record),
    "display_sha256": display_digest,
    "media_analysis_bytes": len(analysis_record),
    "media_analysis_sha256": hashlib.sha256(analysis_record).hexdigest(),
}

# Independent length oracle for the 64x64 ACES handoff fixture. Pixel/config hashes
# have fixed length; their values do not affect these sizes.
process_bytes = (34 + 2 + 3*8 + 2*8 + 8 + 1 + 2*(2*8+2*4) + 2*4
                 + len(text("ACEScg")) + 32 + 3 + len(text("bloom.process.rgba32f.semantic.v2"))
                 + 4*4 + 32)
pixels = "height=u:64;packing=id:rgba;sample-type=id:binary32;width=u:64"
window = "height=u:64;origin-x=i:0;origin-y=i:0;width=u:64"
alpha = "association=id:premultiplied;zero-alpha=id:canonical-zero"
channels = ("count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
            "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha")
none = "kind=id:none;revision=id:none"
exr_facets = [
    ("exr.output-color-transform", pixels, pixels),
    ("", "component-type=id:binary32", "component-type=id:binary32"),
    ("exr.output-color-transform", "color-id=id:ACEScg", "color-id=id:ACES2065-1"),
    ("", alpha, alpha), ("", channels, channels),
    ("", window, window), ("", window, window),
    ("", "denominator=u:1;numerator=u:1", "value=f32:3f800000"),
    ("", "", "method=id:piz"), ("", "profile=id:none", "profile=id:none"),
    ("exr.ocio-external-reference", none, "kind=id:ocio;revision=id:" + "0"*64),
]
look_bytes = len(b"BloomOutputLookV1\0") + 1 + 8
analysis_bytes = (len(b"BloomOutputAnalysisDigest\0") + 2 + 4 + process_bytes
                  + len(text("FlatExrRgba32fLinRec709SceneV1")) + 4
                  + len(text("bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1"))
                  + 4 + 4 + 2 + sum(3 + sum(len(text(v)) for v in facet) for facet in exr_facets)
                  + look_bytes)
assert process_bytes == 271
assert analysis_bytes == 1669
records.update({"aces_process_bytes": process_bytes,
                "handoff_analysis_bytes": analysis_bytes,
                "look_suffix_bytes": look_bytes})
print(json.dumps(records, indent=2))
```
