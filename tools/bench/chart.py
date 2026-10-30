#!/usr/bin/env python3
"""Bloom colour and format accuracy chart: an original, procedural test chart with exact values.

Nothing here is rendered: every pixel comes from a formula, so the expected value of every patch is
known exactly in its source encoding (sRGB-transfer R'G'B' for PNG and SDR video), in linear
Rec.709 (the EXR files and Bloom's linear working space), and as Y'CbCr code values for each video
variant. `chart_truth.json` (written next to this file with --write-truth) carries those values.

    python3 tools/bench/chart.py --out <media-root> [--tiers 1080p,2160p,4320p] [--frames 24]
    python3 tools/bench/chart.py --out <dir> --preview        # one 1080p frame, PNG8/PNG16/EXR
    python3 tools/bench/chart.py --write-truth                # regenerate chart_truth.json

Encoding conventions (also recorded in the truth file):
- PNG: RGBA, sRGB transfer (IEC 61966-2-1), straight alpha, sRGB chunk. 16-bit code = round(v*65535).
- EXR: linear Rec.709 float RGBA, premultiplied alpha, chromaticities attribute, ZIP compression.
  EXR alone carries values above 1.0 and below 0; PNG and SDR video clip them.
- SDR video: the same sRGB-transfer R'G'B' signal as the PNG, BT.709 primaries, tagged
  transfer iec61966-2-1, matrix and range per variant. Chroma is subsampled here, not by FFmpeg:
  horizontally co-sited [1 2 1]/4 ("left" siting), vertically a 2-tap average for 4:2:0.
- HDR video: linear Rec.709 -> BT.2020 primaries, reference white (1.0) = 203 cd/m2 (BT.2408),
  PQ (SMPTE ST 2084) or HLG (BT.2100, reference white at 75% signal). Alpha is ignored by video
  without an alpha plane, which shows the straight colour.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from imageio_min import write_exr, write_png  # noqa: E402

CHART_VERSION = 1
FPS = 24
FRAMES = 24
TIERS = {"1080p": (1920, 1080), "2160p": (3840, 2160), "4320p": (7680, 4320)}
REF_W, REF_H = 1920, 1080
BACKGROUND = 0.25  # sRGB-encoded neutral background
TEXT = 0.9

# ColorChecker-style 24-patch set: the widely published sRGB (D65) 8-bit reference values of the
# classic 24-patch chart, row by row.
REFERENCE_24 = [
    ("dark skin", (115, 82, 68)), ("light skin", (194, 150, 130)), ("blue sky", (98, 122, 157)),
    ("foliage", (87, 108, 67)), ("blue flower", (133, 128, 177)), ("bluish green", (103, 189, 170)),
    ("orange", (214, 126, 44)), ("purplish blue", (80, 91, 166)), ("moderate red", (193, 90, 99)),
    ("purple", (94, 60, 108)), ("yellow green", (157, 188, 64)), ("orange yellow", (224, 163, 46)),
    ("blue", (56, 61, 150)), ("green", (70, 148, 73)), ("red", (175, 54, 60)),
    ("yellow", (231, 199, 31)), ("magenta", (187, 86, 149)), ("cyan", (8, 133, 161)),
    ("white 9.5", (243, 243, 242)), ("neutral 8", (200, 200, 200)),
    ("neutral 6.5", (160, 160, 160)), ("neutral 5", (122, 122, 121)),
    ("neutral 3.5", (85, 85, 85)), ("black 2", (52, 52, 52)),
]
BARS_75 = [("white 75%", (.75, .75, .75)), ("yellow 75%", (.75, .75, 0)),
           ("cyan 75%", (0, .75, .75)), ("green 75%", (0, .75, 0)),
           ("magenta 75%", (.75, 0, .75)), ("red 75%", (.75, 0, 0)), ("blue 75%", (0, 0, .75))]
FULL_100 = [("red 100%", (1, 0, 0)), ("green 100%", (0, 1, 0)), ("blue 100%", (0, 0, 1)),
            ("cyan 100%", (0, 1, 1)), ("magenta 100%", (1, 0, 1)), ("yellow 100%", (1, 1, 0)),
            ("white 100%", (1, 1, 1)), ("black 0%", (0, 0, 0))]
RANGE_CODES = [0, 1, 2, 4, 8, 16, 235, 239, 247, 251, 254, 255]  # 8-bit-equivalent signal codes
HDR_PATCHES = [
    ("linear 1.0", (1.0, 1.0, 1.0)), ("linear 1.5", (1.5, 1.5, 1.5)), ("linear 2", (2, 2, 2)),
    ("linear 4", (4, 4, 4)), ("linear 8", (8, 8, 8)), ("linear 16", (16, 16, 16)),
    ("hot warm", (4.0, 1.0, 0.25)), ("hot cool", (0.25, 1.0, 4.0)),
    ("negative red", (-0.1, 0.3, 0.6)), ("negative green", (0.6, -0.05, 0.4)),
    ("negative grey", (-0.25, -0.25, -0.25)), ("tiny 1e-4", (1e-4, 1e-4, 1e-4)),
]
ALPHA_STEPS = [0.0, 0.25, 0.5, 0.75, 1.0]
ALPHA_RAMP_COLOR = (1.0, 0.45, 0.05)  # linear, straight

VIDEO_VARIANTS = {
    # name: (encoder args, pix_fmt, matrix, range, primaries, transfer, container, signal)
    "h264_709_limited": (["-c:v", "libx264", "-preset", "slow", "-crf", "8"], "yuv420p",
                         "bt709", "tv", "bt709", "iec61966-2-1", "mp4", "sdr"),
    "h264_709_full": (["-c:v", "libx264", "-preset", "slow", "-crf", "8"], "yuvj420p",
                      "bt709", "pc", "bt709", "iec61966-2-1", "mp4", "sdr"),
    "h264_601_limited": (["-c:v", "libx264", "-preset", "slow", "-crf", "8"], "yuv420p",
                         "smpte170m", "tv", "bt709", "iec61966-2-1", "mp4", "sdr"),
    "hevc10_709_limited": (["-c:v", "libx265", "-preset", "slow", "-crf", "8", "-tag:v", "hvc1"],
                           "yuv420p10le", "bt709", "tv", "bt709", "iec61966-2-1", "mp4", "sdr"),
    "hevc10_2020_pq": (["-c:v", "libx265", "-preset", "slow", "-crf", "8", "-tag:v", "hvc1"],
                       "yuv420p10le", "bt2020nc", "tv", "bt2020", "smpte2084", "mp4", "pq"),
    "hevc10_2020_hlg": (["-c:v", "libx265", "-preset", "slow", "-crf", "8", "-tag:v", "hvc1"],
                        "yuv420p10le", "bt2020nc", "tv", "bt2020", "arib-std-b67", "mp4", "hlg"),
    "prores_422hq": (["-c:v", "prores_ks", "-profile:v", "3"], "yuv422p10le", "bt709", "tv",
                     "bt709", "iec61966-2-1", "mov", "sdr"),
    "prores_4444": (["-c:v", "prores_ks", "-profile:v", "4", "-alpha_bits", "16"],
                    "yuva444p10le", "bt709", "tv", "bt709", "iec61966-2-1", "mov", "sdr"),
    "dnxhr_hqx": (["-c:v", "dnxhd", "-profile:v", "dnxhr_hqx"], "yuv422p10le", "bt709", "tv",
                  "bt709", "iec61966-2-1", "mov", "sdr"),
}
# H.264 is not produced at 8K (level 6.2 decoders are rare; same rule as the footage tier).
VIDEO_SKIP = {"4320p": {"h264_709_limited", "h264_709_full", "h264_601_limited"}}
MATRICES = {"bt709": (0.2126, 0.0722), "smpte170m": (0.299, 0.114), "bt2020nc": (0.2627, 0.0593)}
SUGGESTED_TOLERANCE = {"png16": 0, "png8": 0, "exr": 0.0, "video8": 3, "video10": 12}

# --- transfer functions -------------------------------------------------------------------------


def srgb_encode(linear):
    linear = np.clip(np.asarray(linear, dtype=np.float64), 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)


def srgb_decode(signal):
    signal = np.asarray(signal, dtype=np.float64)
    return np.where(signal <= 0.04045, signal / 12.92, ((signal + 0.055) / 1.055) ** 2.4)


REC709_TO_2020 = np.array([[0.6274040, 0.3292820, 0.0433136],
                           [0.0690970, 0.9195400, 0.0113612],
                           [0.0163916, 0.0880132, 0.8955950]])


def pq_encode(linear709):
    rgb = np.clip(np.asarray(linear709, dtype=np.float64) @ REC709_TO_2020.T, 0.0, None)
    y = np.clip(rgb * 203.0 / 10000.0, 0.0, 1.0)
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    return ((c1 + c2 * y ** m1) / (1 + c3 * y ** m1)) ** m2


def hlg_encode(linear709):
    rgb = np.clip(np.asarray(linear709, dtype=np.float64) @ REC709_TO_2020.T, 0.0, None)
    e = np.clip(rgb * 0.26496256, 0.0, 1.0)  # reference white -> 75% HLG signal (BT.2408)
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(e <= 1 / 12, np.sqrt(3 * e), a * np.log(np.maximum(12 * e - b, 1e-12)) + c)


def to_ycbcr(signal, matrix):
    kr, kb = MATRICES[matrix]
    kg = 1 - kr - kb
    r, g, b = signal[..., 0], signal[..., 1], signal[..., 2]
    y = kr * r + kg * g + kb * b
    return y, (b - y) / (2 * (1 - kb)), (r - y) / (2 * (1 - kr))


def quantise(y, cb, cr, bits, full):
    scale = 1 << (bits - 8)
    top = (1 << bits) - 1
    if full:
        yq = np.round(y * top)
        cq = [np.round(c * top + (1 << (bits - 1))) for c in (cb, cr)]
    else:
        yq = np.round((16 + 219 * y) * scale)
        cq = [np.round((128 + 224 * c) * scale) for c in (cb, cr)]
    return [np.clip(v, 0, top).astype(np.uint16) for v in (yq, *cq)]


def video_codes(variant, signal_sdr, linear):
    _, pix_fmt, matrix, rng, _, _, _, kind = VIDEO_VARIANTS[variant]
    bits = 10 if "10" in pix_fmt else 8
    signal = {"sdr": np.asarray(signal_sdr, dtype=np.float64), "pq": pq_encode(linear),
              "hlg": hlg_encode(linear)}[kind]
    return [int(v) for v in quantise(*to_ycbcr(signal, matrix), bits, rng == "pc")], bits

# --- bitmap font: original 5x7 glyphs, one int per row (bit 4 = left) --------------------------


GLYPHS = {
    "A": (14, 17, 17, 31, 17, 17, 17), "B": (30, 17, 17, 30, 17, 17, 30),
    "C": (14, 17, 16, 16, 16, 17, 14), "D": (30, 17, 17, 17, 17, 17, 30),
    "E": (31, 16, 16, 30, 16, 16, 31), "F": (31, 16, 16, 30, 16, 16, 16),
    "G": (14, 17, 16, 23, 17, 17, 15), "H": (17, 17, 17, 31, 17, 17, 17),
    "I": (14, 4, 4, 4, 4, 4, 14), "J": (7, 2, 2, 2, 2, 18, 12),
    "K": (17, 18, 20, 24, 20, 18, 17), "L": (16, 16, 16, 16, 16, 16, 31),
    "M": (17, 27, 21, 21, 17, 17, 17), "N": (17, 17, 25, 21, 19, 17, 17),
    "O": (14, 17, 17, 17, 17, 17, 14), "P": (30, 17, 17, 30, 16, 16, 16),
    "Q": (14, 17, 17, 17, 21, 18, 13), "R": (30, 17, 17, 30, 20, 18, 17),
    "S": (15, 16, 16, 14, 1, 1, 30), "T": (31, 4, 4, 4, 4, 4, 4),
    "U": (17, 17, 17, 17, 17, 17, 14), "V": (17, 17, 17, 17, 17, 10, 4),
    "W": (17, 17, 17, 21, 21, 21, 10), "X": (17, 17, 10, 4, 10, 17, 17),
    "Y": (17, 17, 17, 10, 4, 4, 4), "Z": (31, 1, 2, 4, 8, 16, 31),
    "0": (14, 17, 19, 21, 25, 17, 14), "1": (4, 12, 4, 4, 4, 4, 14),
    "2": (14, 17, 1, 2, 4, 8, 31), "3": (31, 2, 4, 2, 1, 17, 14),
    "4": (2, 6, 10, 18, 31, 2, 2), "5": (31, 16, 30, 1, 1, 17, 14),
    "6": (6, 8, 16, 30, 17, 17, 14), "7": (31, 1, 2, 4, 8, 8, 8),
    "8": (14, 17, 17, 14, 17, 17, 14), "9": (14, 17, 17, 15, 1, 2, 12),
    " ": (0,) * 7, ".": (0, 0, 0, 0, 0, 12, 12), "-": (0, 0, 0, 31, 0, 0, 0),
    "%": (24, 25, 2, 4, 8, 19, 3), "/": (0, 1, 2, 4, 8, 16, 0), ":": (0, 12, 12, 0, 12, 12, 0),
    "+": (0, 4, 4, 31, 4, 4, 0), "(": (2, 4, 8, 8, 8, 4, 2), ")": (8, 4, 2, 2, 2, 4, 8),
    "<": (2, 4, 8, 16, 8, 4, 2), ">": (8, 4, 2, 1, 2, 4, 8), "=": (0, 0, 31, 0, 31, 0, 0),
    ",": (0, 0, 0, 0, 12, 4, 8), "'": (4, 4, 8, 0, 0, 0, 0), "_": (0, 0, 0, 0, 0, 0, 31),
}

# --- chart model --------------------------------------------------------------------------------


class Chart:
    """One frame: signal (sRGB R'G'B'), linear Rec.709 and straight alpha planes."""

    def __init__(self, width, height):
        self.width, self.height, self.s = width, height, width // REF_W
        self.signal = np.full((height, width, 3), BACKGROUND, dtype=np.float64)
        self.linear = np.full((height, width, 3), float(srgb_decode(BACKGROUND)), np.float64)
        self.alpha = np.ones((height, width), dtype=np.float64)
        self.patches = []

    def r(self, x, y, w, h):
        s = self.s
        return (x * s, y * s, w * s, h * s)

    def fill_signal(self, rect, value):
        x, y, w, h = rect
        self.signal[y:y + h, x:x + w] = value
        self.linear[y:y + h, x:x + w] = srgb_decode(np.asarray(value, dtype=np.float64))

    def fill_linear(self, rect, value):
        x, y, w, h = rect
        self.linear[y:y + h, x:x + w] = value
        self.signal[y:y + h, x:x + w] = srgb_encode(np.asarray(value, dtype=np.float64))

    def patch(self, region, name, rect, signal=None, linear=None, alpha=1.0):
        if linear is None:
            self.fill_signal(rect, signal)
            linear = tuple(float(v) for v in srgb_decode(np.asarray(signal, dtype=np.float64)))
        else:
            self.fill_linear(rect, linear)
            signal = tuple(float(v) for v in srgb_encode(np.asarray(linear, dtype=np.float64)))
        x, y, w, h = rect
        self.alpha[y:y + h, x:x + w] = alpha
        inset = max(4 * self.s, min(w, h) // 5)
        self.patches.append({
            "region": region, "name": name, "rect": list(rect),
            "sample": [x + inset, y + inset, max(1, w - 2 * inset), max(1, h - 2 * inset)],
            "signal": [float(v) for v in signal], "linear": [float(v) for v in linear],
            "alpha": float(alpha)})

    def text(self, x, y, string, scale=None):
        scale = scale or 2 * self.s
        cursor = x
        for char in string.upper():
            rows = GLYPHS.get(char, GLYPHS[" "])
            for row, bits in enumerate(rows):
                for col in range(5):
                    if bits & (16 >> col):
                        px, py = cursor + col * scale, y + row * scale
                        self.fill_signal((px, py, scale, scale), (TEXT,) * 3)
            cursor += 6 * scale


def build_chart(width, height, frame):
    c = Chart(width, height)
    s = c.s
    label = lambda x, y, t: c.text(x * s, y * s, t)  # noqa: E731
    label(40, 6, f"BLOOM COLOUR AND FORMAT CHART V{CHART_VERSION}   {width}X{height}   "
                 f"FRAME {frame + 1:02d}/{FRAMES}")
    # 75% bars with 40% side pillars, then 100% primaries and secondaries.
    label(40, 25, "75% COLOUR BARS, 40% GREY PILLARS (BT.709 R'G'B' SIGNAL)")
    c.patch("bars", "pillar left 40%", c.r(0, 40, 240, 160), signal=(.4, .4, .4))
    c.patch("bars", "pillar right 40%", c.r(1680, 40, 240, 160), signal=(.4, .4, .4))
    for index, (name, value) in enumerate(BARS_75):
        c.patch("bars", name, c.r(240 + index * 205, 40, 205 if index < 6 else 210, 160),
                signal=value)
    label(40, 205, "100% PRIMARIES, SECONDARIES, WHITE AND BLACK")
    for index, (name, value) in enumerate(FULL_100):
        c.patch("full", name, c.r(index * 240, 220, 240, 70), signal=value)
    # Grey step wedge and range codes.
    label(40, 295, "GREY STEPS 0-100%")
    label(980, 295, "RANGE CODES 0 1 2 4 8 16 / 235 239 247 251 254 255")
    for index in range(11):
        c.patch("grey", f"grey {index * 10}%", c.r(40 + index * 82, 310, 80, 60),
                signal=(index / 10,) * 3)
    for index, code in enumerate(RANGE_CODES):
        c.patch("range", f"code {code}", c.r(980 + index * 75, 310, 74, 60),
                signal=(code / 255,) * 3)
    # Smooth ramps: signal-linear, light-linear, then R, G and B.
    label(40, 375, "RAMPS: SIGNAL 0-1, LINEAR LIGHT 0-1, RED, GREEN, BLUE")
    x0, x1 = 40 * s, 1880 * s
    t = np.arange(x1 - x0, dtype=np.float64) / (x1 - x0 - 1)
    ramps = [("signal", 390, 40, np.stack([t] * 3, axis=1), False),
             ("light", 432, 40, np.stack([t] * 3, axis=1), True),
             ("red", 474, 12, np.stack([t, 0 * t, 0 * t], axis=1), False),
             ("green", 488, 12, np.stack([0 * t, t, 0 * t], axis=1), False),
             ("blue", 502, 12, np.stack([0 * t, 0 * t, t], axis=1), False)]
    ramp_truth = []
    for name, y, h, values, light in ramps:
        rows = slice(y * s, (y + h) * s)
        if light:
            c.linear[rows, x0:x1] = values
            c.signal[rows, x0:x1] = srgb_encode(values)
        else:
            c.signal[rows, x0:x1] = values
            c.linear[rows, x0:x1] = srgb_decode(values)
        ramp_truth.append({"name": name, "rect": [x0, y * s, x1 - x0, h * s],
                           "definition": ("linear = column / (width - 1)" if light else
                                          "signal = column / (width - 1)")})
    # 24-patch reference set.
    label(40, 521, "24 PATCH REFERENCE SET (SRGB 8-BIT)")
    for index, (name, rgb) in enumerate(REFERENCE_24):
        col, row = index % 6, index // 6
        c.patch("reference24", name, c.r(40 + col * 120, 536 + row * 60, 112, 52),
                signal=tuple(v / 255 for v in rgb))
    pixel_truth = pixel_structures(c, label)
    # EXR-only values and alpha.
    label(40, 783, "EXR ONLY: ABOVE 1.0 AND BELOW 0 (PNG AND VIDEO CLIP)")
    for index, (name, value) in enumerate(HDR_PATCHES):
        col, row = index % 6, index // 6
        c.patch("hdr", name, c.r(40 + col * 120, 798 + row * 80, 112, 72), linear=value)
    label(800, 783, "ALPHA: RAMP AND STEPS (STRAIGHT IN PNG, PREMULTIPLIED IN EXR)")
    ax0, ax1 = 800 * s, 1880 * s
    a = np.arange(ax1 - ax0, dtype=np.float64) / (ax1 - ax0 - 1)
    for y, color in ((798, ALPHA_RAMP_COLOR), (852, (1.0, 1.0, 1.0))):
        rows = slice(y * s, (y + 50) * s)
        c.linear[rows, ax0:ax1] = color
        c.signal[rows, ax0:ax1] = srgb_encode(np.asarray(color))
        c.alpha[rows, ax0:ax1] = a
    for index, value in enumerate(ALPHA_STEPS):
        c.patch("alpha", f"alpha {value}", c.r(800 + index * 216, 906, 208, 52),
                linear=(1.0, 1.0, 1.0), alpha=value)
    # Motion strip: one square moving one step per frame, frame counter at the right.
    label(40, 966, "MOTION")
    travel = (1600 - 40) * s
    mx = 40 * s + (travel * frame) // (FRAMES - 1)
    my = 990 * s
    c.fill_signal((40 * s, my - 4 * s, 1640 * s, 64 * s), (0.1, 0.1, 0.1))
    c.fill_signal((mx, my, 48 * s, 48 * s), (0.95, 0.95, 0.95))
    c.fill_signal((mx + 12 * s, my + 12 * s, 24 * s, 24 * s), (0.9, 0.05, 0.05))
    label(1700, 1010, f"FRAME {frame + 1:02d}")
    label(40, 1062, "ORIGINAL PROCEDURAL DESIGN - EXPECTED VALUES IN CHART_TRUTH.JSON")
    return c, {"ramps": ramp_truth, "pixel": pixel_truth,
               "alphaRamps": [{"rect": [ax0, 798 * s, ax1 - ax0, 50 * s],
                               "linearStraight": list(ALPHA_RAMP_COLOR),
                               "definition": "alpha = column / (width - 1)"},
                              {"rect": [ax0, 852 * s, ax1 - ax0, 50 * s],
                               "linearStraight": [1.0, 1.0, 1.0],
                               "definition": "alpha = column / (width - 1)"}],
               "motion": {"square": [mx, my, 48 * s, 48 * s], "signal": [0.95] * 3}}


def pixel_structures(c, label):
    """Device-pixel patterns: they do not scale with the tier, their boxes do."""
    s = c.s
    y0, h = 536 * s, 240 * s
    yy, xx = np.mgrid[0:h, 0:200 * s]
    boxes = []

    def put(x_ref, pattern, name, definition):
        x = x_ref * s
        ph, pw = pattern.shape
        c.signal[y0:y0 + ph, x:x + pw] = pattern[..., None]
        c.linear[y0:y0 + ph, x:x + pw] = srgb_decode(pattern)[..., None]
        boxes.append({"name": name, "rect": [x, y0, pw, ph], "definition": definition})

    label(800, 521, "1PX V")
    put(800, (xx % 2 == 0).astype(np.float64), "vertical 1px lines",
        "signal 1.0 on even image columns, 0.0 on odd")
    label(1010, 521, "1PX H")
    put(1010, (yy % 2 == 0).astype(np.float64), "horizontal 1px lines",
        "signal 1.0 on even image rows, 0.0 on odd")
    label(1220, 521, "CHECK 1/2")
    check = np.where(yy < h // 2, (xx + yy) % 2 == 0, ((xx // 2) + (yy // 2)) % 2 == 0)
    put(1220, check.astype(np.float64), "checkerboards",
        "top half 1px checker, bottom half 2px checker; 1.0 where (x+y) even")
    label(1430, 521, "ZONE PLATE")
    size = 200 * s
    zy, zx = np.mgrid[0:size, 0:size].astype(np.float64)
    radius = size / 2
    r2 = (zx - radius + 0.5) ** 2 + (zy - radius + 0.5) ** 2
    zone = 0.5 + 0.5 * np.cos(np.pi * r2 / (2 * radius))  # reaches Nyquist at the edge
    put(1430, zone, "zone plate", "0.5 + 0.5 cos(pi r^2 / size), r from the box centre")
    label(1640, 521, "SITING")
    # Tiny saturated squares on a dark field, at even and odd columns and rows.
    sx, field = 1640 * s, (0.1, 0.1, 0.1)
    c.fill_signal((sx, y0, 240 * s, h), field)
    squares = []
    colours = [("red", (1, 0, 0)), ("green", (0, 1, 0)), ("blue", (0, 0, 1)),
               ("magenta", (1, 0, 1))]
    for row, (cname, colour) in enumerate(colours):
        for col, (size_px, dx, dy) in enumerate([(1, 0, 0), (1, 1, 0), (1, 0, 1), (1, 1, 1),
                                                 (2, 0, 0), (2, 1, 0), (2, 0, 1), (2, 1, 1)]):
            x = sx + 16 + col * 26 * s + dx
            y = y0 + 16 + row * 56 * s + dy
            c.fill_signal((x, y, size_px, size_px), colour)
            squares.append({"colour": cname, "signal": list(colour), "rect": [x, y, size_px,
                                                                               size_px],
                            "columnParity": "odd" if x % 2 else "even",
                            "rowParity": "odd" if y % 2 else "even"})
    return {"boxes": boxes, "sitingField": {"rect": [sx, y0, 240 * s, h], "signal": list(field)},
            "sitingSquares": squares}

# --- outputs ------------------------------------------------------------------------------------


def png_codes(chart, bits):
    top = (1 << bits) - 1
    rgb = np.round(np.clip(chart.signal, 0, 1) * top)
    alpha = np.round(chart.alpha * top)[..., None]
    return np.concatenate([rgb, alpha], axis=2).astype(np.uint16)


def viewing_codes(chart, square=16):
    """Opaque 8-bit viewing copy: composited in linear light over a grey checkerboard."""
    yy, xx = np.mgrid[0:chart.height, 0:chart.width]
    board = np.where(((xx // square) + (yy // square)) % 2 == 0, 0.6, 0.4)
    under = srgb_decode(board)[..., None]
    alpha = chart.alpha[..., None]
    signal = srgb_encode(chart.linear * alpha + under * (1 - alpha))
    rgb = np.round(signal * 255)
    return np.concatenate([rgb, np.full_like(rgb[..., :1], 255)], axis=2).astype(np.uint16)


def exr_channels(chart):
    premultiplied = chart.linear * chart.alpha[..., None]
    return {"R": premultiplied[..., 0], "G": premultiplied[..., 1], "B": premultiplied[..., 2],
            "A": chart.alpha}


def subsample(plane, pix_fmt):
    if pix_fmt.startswith(("yuv444", "yuva444")):
        return plane
    padded = np.pad(plane, ((0, 0), (1, 1)), mode="edge")
    horizontal = (padded[:, 0:-2:2] + 2 * padded[:, 1:-1:2] + padded[:, 2::2]) / 4  # co-sited
    if pix_fmt.startswith(("yuv420", "yuvj420")):
        return (horizontal[0::2] + horizontal[1::2]) / 2
    return horizontal


def video_frame(chart, variant):
    _, pix_fmt, matrix, rng, _, _, _, kind = VIDEO_VARIANTS[variant]
    signal = {"sdr": np.clip(chart.signal, 0, 1), "pq": pq_encode(chart.linear),
              "hlg": hlg_encode(chart.linear)}[kind]
    y, cb, cr = to_ycbcr(signal, matrix)
    cb, cr = subsample(cb, pix_fmt), subsample(cr, pix_fmt)
    bits = 10 if "10" in pix_fmt else 8
    planes = quantise(y, cb, cr, bits, rng == "pc")
    if pix_fmt.startswith("yuva"):
        planes.append(np.round(chart.alpha * ((1 << bits) - 1)).astype(np.uint16))
    dtype = "<u2" if bits > 8 else "u1"
    return b"".join(p.astype(dtype).tobytes() for p in planes)


def encode_video(path, frames, width, height, variant, ffmpeg="ffmpeg"):
    args, pix_fmt, matrix, rng, primaries, transfer, _, _ = VIDEO_VARIANTS[variant]
    raw_fmt = pix_fmt.replace("yuvj", "yuv")
    tags = ["-color_primaries", primaries, "-color_trc", transfer, "-colorspace", matrix,
            "-color_range", rng, "-chroma_sample_location", "left"]
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
               "-pix_fmt", raw_fmt, "-s", f"{width}x{height}", "-r", str(FPS), *tags, "-i", "-",
               "-vf", (f"setparams=color_primaries={primaries}:color_trc={transfer}:"
                       f"colorspace={matrix}:range={rng}:chroma_location=left"),
               *args, "-pix_fmt", raw_fmt, *tags, "-an", str(path)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, env={**os.environ, "SVT_LOG": "1"})
    for frame in frames:
        process.stdin.write(frame)
    process.stdin.close()
    if process.wait() != 0:
        raise SystemExit(f"ffmpeg failed for {path}")


def truth(width, height):
    chart, extra = build_chart(width, height, 0)
    patches = []
    for patch in chart.patches:
        entry = dict(patch)
        entry["png16"] = [int(round(min(max(v, 0), 1) * 65535)) for v in patch["signal"]]
        entry["png16"].append(int(round(patch["alpha"] * 65535)))
        entry["exrPremultiplied"] = [v * patch["alpha"] for v in patch["linear"]] + [
            patch["alpha"]]
        entry["video"] = {}
        for variant in VIDEO_VARIANTS:
            codes, bits = video_codes(variant, np.clip(patch["signal"], 0, 1),
                                      np.asarray(patch["linear"]))
            entry["video"][variant] = codes
        patches.append(entry)
    return {"size": [width, height], "patches": patches, **extra}


def truth_document():
    return {
        "chart": "bloom-colour-format-chart", "version": CHART_VERSION, "frames": FRAMES,
        "fps": FPS, "frameNumbering": "files are numbered 0001..0024",
        "conventions": {
            "signal": "sRGB transfer (IEC 61966-2-1) R'G'B' in 0..1; PNG and SDR video carry it",
            "linear": "linear Rec.709 (D65), straight alpha; Bloom's linear working space",
            "png": "RGBA, straight alpha, code = round(clamp(signal) * (2^bits - 1))",
            "exr": "float RGBA, premultiplied, ZIP; values outside 0..1 preserved",
            "videoSdr": "signal -> Y'CbCr by the variant's matrix and range; chroma co-sited "
                        "horizontally ([1 2 1]/4), 4:2:0 vertical 2-tap average; tags "
                        "primaries bt709, transfer iec61966-2-1",
            "videoHdr": "linear Rec.709 -> BT.2020, 1.0 = 203 cd/m2; PQ ST 2084 or HLG BT.2100 "
                        "(1.0 -> 75% signal); negative values clip after the primaries change",
            "videoCodes": "[Y, Cb, Cr] integer codes at the variant's bit depth",
            "videoAlpha": "video without alpha shows the straight colour",
            "sample": "rectangle [x, y, w, h] inset from the patch edges; average it",
        },
        "suggestedTolerance": SUGGESTED_TOLERANCE,
        "videoVariants": {name: {"pixFmt": v[1], "matrix": v[2], "range": v[3],
                                 "primaries": v[4], "transfer": v[5], "container": v[6],
                                 "signal": v[7]} for name, v in VIDEO_VARIANTS.items()},
        "videoSkipped": {tier: sorted(names) for tier, names in VIDEO_SKIP.items()},
        "tiers": {tier: truth(*size) for tier, size in TIERS.items()},
    }


def write_frames(out_root, tier, frames, formats, videos):
    width, height = TIERS[tier]
    base = Path(out_root) / "chart" / tier
    for name in formats:
        (base / name).mkdir(parents=True, exist_ok=True)
    video_frames = {variant: [] for variant in videos}
    for frame in range(frames):
        chart, _ = build_chart(width, height, frame)
        stem = f"chart_{tier}"
        number = f"{frame + 1:04d}"
        if "png8" in formats:
            write_png(base / "png8" / f"{stem}_png8.{number}.png", png_codes(chart, 8), 8)
        if "png16" in formats:
            write_png(base / "png16" / f"{stem}_png16.{number}.png", png_codes(chart, 16), 16)
        if "exr_float_zip" in formats:
            write_exr(base / "exr_float_zip" / f"{stem}_exr_float_zip.{number}.exr",
                      exr_channels(chart))
        for variant in videos:
            video_frames[variant].append(video_frame(chart, variant))
        print(f"chart {tier} frame {frame + 1}/{frames}", flush=True)
    if videos:
        (base / "video").mkdir(parents=True, exist_ok=True)
    for variant in videos:
        container = VIDEO_VARIANTS[variant][6]
        encode_video(base / "video" / f"chart_{tier}_{variant}.{container}", video_frames[variant],
                     width, height, variant)
        print(f"chart {tier} video {variant}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", help="media root (chart files go to <out>/chart/<tier>/)")
    parser.add_argument("--tiers", default=",".join(TIERS))
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--no-video", action="store_true")
    parser.add_argument("--preview", action="store_true",
                        help="write one 1080p frame as PNG8/PNG16/EXR into --out")
    parser.add_argument("--write-truth", action="store_true",
                        help="write chart_truth.json next to this script")
    args = parser.parse_args()
    if args.write_truth:
        path = Path(__file__).resolve().parent / "chart_truth.json"
        path.write_text(json.dumps(truth_document(), separators=(",", ":")) + "\n")
        print(f"wrote {path} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}")
    if args.preview:
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        chart, _ = build_chart(1920, 1080, 0)
        write_png(out / "chart_1080p_preview_srgb8.png", viewing_codes(chart), 8)
        write_png(out / "chart_1080p_png16.0001.png", png_codes(chart, 16), 16)
        write_exr(out / "chart_1080p_exr_float_zip.0001.exr", exr_channels(chart))
        print(f"preview written to {out}")
        return
    if args.out:
        for tier in [t for t in args.tiers.split(",") if t]:
            videos = [] if args.no_video else [v for v in VIDEO_VARIANTS
                                               if v not in VIDEO_SKIP.get(tier, set())]
            write_frames(args.out, tier, min(args.frames, FRAMES), ("png16", "exr_float_zip"),
                         videos)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    main()
