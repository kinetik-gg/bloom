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
print(json.dumps({
    "display_bytes": len(display_record),
    "display_sha256": display_digest,
    "media_analysis_bytes": len(analysis_record),
    "media_analysis_sha256": hashlib.sha256(analysis_record).hexdigest(),
}, indent=2))
