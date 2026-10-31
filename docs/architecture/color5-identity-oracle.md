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


def color(name):
    """Colour-space names are OCIO free text: the Color facet carries their UTF-8 bytes as hex."""
    return "color-id=utf8:" + name.encode("utf-8").hex()


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
    + bytes.fromhex("7e8bea8770830d7257eba98ee80d465a1d0106bbc3e06665f8d171d99b1baa2f")
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
    ("exr.output-color-transform", color("ACEScg"), color("ACES2065-1")),
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
assert analysis_bytes == 1689
records.update({"aces_process_bytes": process_bytes,
                "handoff_analysis_bytes": analysis_bytes,
                "look_suffix_bytes": look_bytes})
print(json.dumps(records, indent=2))
```

## Output identity oracle (plan semantics 9)

ADR 0024 advances compiled-plan semantics 8 -> 9; animation sampling (2), evaluator (10) and image
primitives (7) are unchanged, and no pixel golden moves. The block below imports no Bloom code. It
independently packs the process-frame identity, the display-processor identity, the output analysis
digest and the output semantic identity for the 1x1 test fixture, and prints the digests pinned by
`output_analysis_digest_tests.cpp`, `output_analysis_analyzer_tests.cpp` and
`output_semantic_identity_tests.cpp`. Run it as `python3 -I oracle.py PLAN ANIMATION EVALUATOR
PRIMITIVES`.

Procedure and results for this change:

- With versions `8 2 9 7` it reproduces the previously checked-in pins byte for byte (PNG analysis
  `a9b4052a...`, EXR analysis `2eda964b...`, PNG output `8a56350e...`, EXR output `82422afc...`). Those
  pins pre-date the evaluator 9 -> 10 change, so the checked-in set was one evaluator step stale.
- Fed the golden fixture of `process_frame_semantic_identity_tests.cpp` it reproduces the pixel
  digest `db7f7d3d...` and the 249-byte identity digest `a403a32e...`.
- With versions `9 2 10 7` it derives the replacements, which the C++ implementation then matched:
  PNG analysis `9b8de77e...` (1922 bytes), EXR analysis `2c124174...` (1485), PNG output
  `e125d85a...` (669) and EXR output `fa7183ae...` (567). Preimage lengths are unchanged.

ADR 0026 later advances compiled-plan semantics 9 -> 10 and evaluator semantics 10 -> 11. With
`9 2 10 7` the same oracle reproduces the pins above; with `10 2 11 7` it derives the replacements,
which the C++ implementation matched: PNG analysis `91fb3c3f...` (1922 bytes), EXR analysis
`7975de4c...` (1485), PNG output `89db168a...` (669) and EXR output `3987dbcb...` (567).

The Color facet then changed its color-space encoding from `color-id=id:<name>` to
`color-id=utf8:<lower-hex UTF-8 bytes>`: OCIO color-space names are free text (for example
`Linear Rec.709 (sRGB)`), and the `id:` grammar refused them, so every export from such a working
space failed its analysis. The blocks above encode names with `color()`. With `10 2 11 7` the oracle
derives PNG analysis `f8d94bef...` (1961 bytes), EXR analysis `45c89367...` (1521), PNG output
`c9eabd7d...` (669) and EXR output `877e4cb0...` (567), and the ACES handoff record grows from 1669
to 1689 bytes; the C++ implementation matches all of them. Analysis digests and output identities
live only within one export run (approval and reopen verification), so no saved project, cache or
published file carries the previous values.

Keyframe easing then advanced animation sampling 2 -> 3 (`kAnimationSamplingSemanticsVersion`),
another frozen u32 of the process-frame identity, without re-deriving these pins. With `10 3 11 7`
the oracle derives PNG analysis `b6ab5ab6...` (1961 bytes), EXR analysis `469bd805...` (1521), PNG
output `76b216b3...` (669) and EXR output `943d71de...` (567); the C++ implementation matches all
of them and every preimage length is unchanged.

```python
"""Independent output-identity oracle (no Bloom code imported). Usage: oracle.py PLAN ANIM EVAL PRIM"""
import hashlib, struct, sys
sha = lambda b: hashlib.sha256(b).hexdigest()
u8 = lambda v: struct.pack(">B", v); u16 = lambda v: struct.pack(">H", v)
u32 = lambda v: struct.pack(">I", v); u64 = lambda v: struct.pack(">Q", v)
i32 = lambda v: struct.pack(">i", v); i64 = lambda v: struct.pack(">q", v)
text = lambda s: u32(len(s.encode())) + s.encode()
blob = lambda b: u32(len(b)) + b
color = lambda name: "color-id=utf8:" + name.encode().hex()

def process_identity(pixels_f32_bits, versions, project=1, comp=2, rev=6, tnum=0, tden=1, node=4,
                     w=1, h=1):
    plan, anim, ev, prim = versions
    px = b"BloomProcessPixelStream\0" + u16(1) + u64(w*h) + b"".join(u32(b) for b in pixels_f32_bits)
    pdig = hashlib.sha256(px).digest()
    win = i64(0)+i64(0)+u32(w)+u32(h)
    b = (b"BloomProcessFrameSemanticIdentity\0" + u16(1) + u64(project)+u64(comp)+u64(rev)
         + i64(tnum)+i64(tden) + u64(node) + u8(1) + win + win + u32(1)+u32(1)
         + text("lin_rec709_scene") + u8(1)+u8(1)+u8(1) + text("bloom.process.rgba32f.semantic.v2")
         + u32(plan)+u32(anim)+u32(ev)+u32(prim) + pdig)
    return b

def f32bits(x): return struct.unpack(">I", struct.pack(">f", x))[0]
REV = bytes(range(32))
def display_identity():
    return (b"BloomDisplayProcessorIdentity\0" + u16(1) + REV + u16(1) + text("A")+text("B")
            + text("lin_rec709_scene") + text("D") + text("V") + u8(1) + u16(2) + text("L")+text("M")
            + text("srgb_rec709_display") + text("reference") + text("bloom.color.ocio-cpu-display.v1")
            + text("straight-rgba8"))

CH = ("count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
      "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha")
SRC = "height=u:1;packing=id:rgba;sample-type=id:binary32;width=u:1"
PNGP = "height=u:1;packing=id:rgba;sample-type=id:uint8;width=u:1"
WIN = "height=u:1;origin-x=i:0;origin-y=i:0;width=u:1"
NONE = "kind=id:none;revision=id:none"
OCIO = "kind=id:ocio;revision=id:" + REV.hex()
PREM = "association=id:premultiplied;zero-alpha=id:canonical-zero"
STR = "association=id:straight;zero-alpha=id:canonical-zero"
exr_facets = [(1,1,"",SRC,SRC),(2,1,"","component-type=id:binary32","component-type=id:binary32"),
 (3,1,"",color("lin_rec709_scene"),color("lin_rec709_scene")),(4,1,"",PREM,PREM),
 (5,1,"",CH,CH),(6,1,"",WIN,WIN),(7,1,"",WIN,WIN),
 (8,1,"","denominator=u:1;numerator=u:1","value=f32:3f800000"),(9,1,"","","method=id:zip"),
 (10,1,"","profile=id:none","profile=id:none"),(11,1,"",NONE,NONE)]
def analysis_digest(proc, preset, ver, profile, facets, revb=b"", disp=b""):
    r = (b"BloomOutputAnalysisDigest\0" + u16(1) + blob(proc) + text(preset) + u32(ver) + text(profile)
         + blob(revb) + blob(disp) + u16(len(facets)))
    for fid, st, code, s, t in facets:
        r += u8(fid) + u8(st) + u8(1) + text(code) + text(s) + text(t)
    return r

png_facets = [(1,3,"png.display-transform-clamp-quantize",SRC,PNGP),
 (2,3,"png.float32-to-uint8","component-type=id:binary32","component-type=id:uint8"),
 (3,3,"png.lin-rec709-scene-to-srgb",color("lin_rec709_scene"),color("srgb_rec709_display")),
 (4,3,"png.premultiplied-to-straight",PREM,STR),(5,1,"",CH,CH),(6,1,"",WIN,WIN),(7,1,"",WIN,WIN),
 (8,1,"","denominator=u:1;numerator=u:1","denominator=u:1;numerator=u:1"),
 (9,1,"","","method=id:deflate-level-6-filter-none"),(10,1,"","profile=id:none","profile=id:none"),
 (11,5,"png.ocio-external-reference",NONE,OCIO)]
PNGID=("PngRgba8SrgbV1","bloom.output.png-rgba8-srgb.semantic.v1")
EXRID=("FlatExrRgba32fLinRec709SceneV1","bloom.output.exr-rgba32f-lin-rec709-scene.semantic.v1")
CHROMA=[0x3F23D70A,0x3EA8F5C3,0x3E99999A,0x3F19999A,0x3E19999A,0x3D75C28F,0x3EA01A37,0x3EA872B0]

def run(versions):
    out = {}
    tiny = [f32bits(x) for x in (0.25,0.5,0.75,1.0)]
    proc = process_identity(tiny, versions)
    ar = analysis_digest(proc, PNGID[0], 1, PNGID[1], png_facets, REV, display_identity())
    out["png_analysis"] = (sha(ar), len(ar))
    ae = analysis_digest(proc, EXRID[0], 1, EXRID[1], exr_facets)
    out["exr_analysis(tiny)"] = (sha(ae), len(ae))
    # PNG output identity
    payload = bytes([0,0x7F,0x80,0xFF])
    pr = (b"BloomOutputSemanticIdentity\0" + u16(1) + hashlib.sha256(ar).digest() + blob(proc)
          + text(PNGID[0]) + u32(1) + text(PNGID[1]) + blob(display_identity()) + u8(1)
          + u32(1)+u32(1)+u8(1)+u8(1)+text("srgb_rec709_display")+u8(0)
          + text("png.ihdr-srgb-idat-iend.v1") + u64(len(payload)) + payload)
    out["png_output"] = (sha(pr), len(pr))
    # EXR output identity: frame {-0,-1,10,1}
    bits = [0x80000000,0xBF800000,0x41200000,0x3F800000]
    proc2 = process_identity(bits, versions)
    ae2 = analysis_digest(proc2, EXRID[0], 1, EXRID[1], exr_facets)
    er = (b"BloomOutputSemanticIdentity\0" + u16(1) + hashlib.sha256(ae2).digest() + blob(proc2)
          + text(EXRID[0]) + u32(1) + text(EXRID[1]) + blob(b"") + u8(2)
          + i32(0)*8 + u32(0x3F800000) + u8(1) + u8(1) + text("lin_rec709_scene")
          + b"".join(u32(c) for c in CHROMA) + text("exr.singlepart-scanline-rgba32f.v1") + u64(1)
          + b"".join(u32(b) for b in bits))
    out["exr_output"] = (sha(er), len(er))
    return out

if __name__ == "__main__":
    v = tuple(int(a) for a in sys.argv[1:5])
    for k, (d, n) in run(v).items():
        print(f"{k:20s} {d} preimage={n}")
```
