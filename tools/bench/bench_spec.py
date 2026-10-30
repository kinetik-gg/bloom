"""The benchmark media specification shared by the generator, manifest, project builder and runner.

Plain data only (no bpy, no numpy), so Blender's Python, the system Python and Bloom's embedded
Python can all import it.
"""

MEDIA_ROOT_ENV = "BLOOM_BENCH_MEDIA"
DEFAULT_MEDIA_ROOT = "bench-media"  # relative to the current directory
SEED = 1729
FPS = 24
BLENDER_SERIES = "4.5"  # LTS series the stored generator is pinned to

IMAGE_FORMATS = ("png8", "png16", "exr_half_piz", "exr_float_zip", "exr_multilayer")
VIDEO_FORMATS = ("h264", "hevc10", "prores422hq", "prores4444", "dnxhr_hqx", "av1", "vp9")
# tier: (width, height, frames, image formats, video formats)
TIERS = {
    "720p": (1280, 720, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    "1080p": (1920, 1080, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    "2160p": (3840, 2160, 240, IMAGE_FORMATS, VIDEO_FORMATS),
    # Reduced 8K tier: streaming/memory invariant and per-image GPU ceiling stress.
    "4320p": (7680, 4320, 48, ("png16", "exr_half_piz", "exr_multilayer"),
              ("prores4444", "hevc10")),
}
TIER_ORDER = ("720p", "1080p", "2160p", "4320p")

# Pinned in the manifest. Change them only together with regenerated media and manifest.
RENDER_SETTINGS = {
    "engine": "CYCLES",
    "device": "GPU (OptiX)",
    "samples": 128,
    "adaptiveThreshold": 0.02,
    "denoiser": "OPENIMAGEDENOISE (GPU)",
    "maxBounces": 6,
    "seed": SEED,
    "animatedSeed": False,
    "viewTransform": "Standard",
    "filmTransparent": True,
    "passes": ["Combined", "Depth", "Normal", "CryptoObject (4 levels)", "AOV bench_color",
               "AOV bench_id", "Noisy Image"],
    "multilayer": "OPEN_EXR_MULTILAYER half ZIP (depth, normal, Cryptomatte, value AOV float)",
    "flat": {"png8": "PNG RGBA 8-bit, compression 15", "png16": "PNG RGBA 16-bit, compression 15",
             "exr_half_piz": "OpenEXR RGBA half PIZ", "exr_float_zip": "OpenEXR RGBA float ZIP"},
    "frameNumbering": "1-based, 4 digits",
}

# Sequence file name inside <root>/<tier>/<format>/ ; '####' is the frame number.
SEQUENCE_PATTERN = {
    "png8": "bench_{tier}_png8.####.png",
    "png16": "bench_{tier}_png16.####.png",
    "exr_half_piz": "bench_{tier}_exr_half_piz.####.exr",
    "exr_float_zip": "bench_{tier}_exr_float_zip.####.exr",
    "exr_multilayer": "bench_{tier}_multilayer.####.exr",
}

# Video encodes from the 16-bit PNG master. Non-alpha codecs flatten over black (premultiply).
# Footage video is tagged BT.709 primaries/transfer/matrix, limited range: the common delivery
# tagging. Colour accuracy is measured with the chart, not with this footage.
_TAGS = ["-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
         "-color_range", "tv"]
VIDEO_ENCODES = {
    # format: (file suffix, alpha, pix_fmt, encoder args)
    "h264": ("h264.mp4", False, "yuv420p",
             ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-profile:v", "high"]),
    "hevc10": ("hevc10.mp4", False, "yuv420p10le",
               ["-c:v", "libx265", "-preset", "medium", "-crf", "20", "-profile:v", "main10",
                "-tag:v", "hvc1"]),
    "prores422hq": ("prores422hq.mov", False, "yuv422p10le",
                    ["-c:v", "prores_ks", "-profile:v", "3", "-vendor", "apl0"]),
    "prores4444": ("prores4444.mov", True, "yuva444p10le",
                   ["-c:v", "prores_ks", "-profile:v", "4", "-alpha_bits", "16",
                    "-vendor", "apl0"]),
    "dnxhr_hqx": ("dnxhr_hqx.mov", False, "yuv422p10le",
                  ["-c:v", "dnxhd", "-profile:v", "dnxhr_hqx"]),
    "av1": ("av1.mp4", False, "yuv420p10le",
            ["-c:v", "libsvtav1", "-preset", "8", "-crf", "30"]),
    "vp9": ("vp9.webm", False, "yuv420p",
            ["-c:v", "libvpx-vp9", "-crf", "31", "-b:v", "0", "-row-mt", "1",
             "-deadline", "good", "-cpu-used", "4"]),
}
VIDEO_TAGS = _TAGS


def tier_size(tier):
    width, height, frames, _, _ = TIERS[tier]
    return width, height, frames


def sequence_path(tier, image_format):
    """Media-root relative pattern with '####' for the frame number."""
    return f"{tier}/{image_format}/" + SEQUENCE_PATTERN[image_format].format(tier=tier)


def video_path(tier, video_format):
    return f"{tier}/video/bench_{tier}_" + VIDEO_ENCODES[video_format][0]


def footage_assets():
    """Every footage asset id in tier order: '<tier>/<format>'."""
    for tier in TIER_ORDER:
        _, _, _, images, videos = TIERS[tier]
        for image_format in images:
            yield tier, image_format, "sequence"
        for video_format in videos:
            yield tier, video_format, "video"
