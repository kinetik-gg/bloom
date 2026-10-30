"""Minimal, dependency-free (numpy only) PNG and OpenEXR writers and header readers.

The benchmark tools use these to write exactly known chart values and to verify generated media
without OpenImageIO. Only the subset the suite needs is implemented: RGBA PNG (8/16-bit), scanline
OpenEXR with HALF/FLOAT channels and NONE/ZIP compression, plus header parsing for PNG and EXR
(including multilayer channel lists and compression).
"""
import struct
import zlib

import numpy as np

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
EXR_MAGIC = 20000630
EXR_COMPRESSION = {0: "NONE", 1: "RLE", 2: "ZIPS", 3: "ZIP", 4: "PIZ", 5: "PXR24", 6: "B44",
                   7: "B44A", 8: "DWAA", 9: "DWAB", 10: "HTJ2K256", 11: "HTJ2K32"}
EXR_PIXEL_TYPES = {0: "UINT", 1: "HALF", 2: "FLOAT"}
REC709_CHROMATICITIES = (0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290)


def _png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def write_png(path, rgba, bit_depth, srgb=True, level=6):
    """rgba: HxWx4 unsigned integer array already quantised to bit_depth."""
    height, width, channels = rgba.shape
    assert channels == 4 and bit_depth in (8, 16)
    data = rgba.astype(">u2" if bit_depth == 16 else "u1").tobytes()
    stride = width * 4 * (bit_depth // 8)
    rows = np.frombuffer(data, dtype=np.uint8).reshape(height, stride)
    # PNG "Sub" filter (type 1) on every row: byte minus the byte one pixel to the left.
    bpp = 4 * (bit_depth // 8)
    filtered = rows.copy()
    filtered[:, bpp:] = rows[:, bpp:] - rows[:, :-bpp]
    raw = np.empty((height, stride + 1), dtype=np.uint8)
    raw[:, 0] = 1
    raw[:, 1:] = filtered
    chunks = [_png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, 6, 0, 0, 0))]
    if srgb:
        chunks.append(_png_chunk(b"sRGB", b"\x00"))
    chunks.append(_png_chunk(b"IDAT", zlib.compress(raw.tobytes(), level)))
    chunks.append(_png_chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(PNG_SIGNATURE + b"".join(chunks))


def read_png_header(path):
    with open(path, "rb") as handle:
        head = handle.read(33)
    if head[:8] != PNG_SIGNATURE or head[12:16] != b"IHDR":
        raise ValueError(f"{path}: not a PNG")
    width, height, depth, color_type = struct.unpack(">IIBB", head[16:26])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    return {"format": "png", "width": width, "height": height, "bitDepth": depth,
            "channels": channels}


def _attr(name, kind, payload):
    return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(payload)) + payload


def _zip_encode(raw):
    data = np.frombuffer(raw, dtype=np.uint8)
    interleaved = np.concatenate([data[0::2], data[1::2]])
    predicted = interleaved.copy()
    predicted[1:] = (interleaved[1:].astype(np.int16) - interleaved[:-1].astype(np.int16) +
                     128).astype(np.uint8)
    return zlib.compress(predicted.tobytes(), 6)


def write_exr(path, channels, half=False, compression="ZIP", chromaticities=REC709_CHROMATICITIES):
    """channels: dict name -> HxW float array. Scanline file, sorted channel order."""
    names = sorted(channels)
    height, width = channels[names[0]].shape
    pixel_type = 1 if half else 2
    dtype = np.dtype("<f2" if half else "<f4")
    chlist = b"".join(name.encode() + b"\0" + struct.pack("<iB3xii", pixel_type, 0, 1, 1)
                      for name in names) + b"\0"
    code = {"NONE": 0, "ZIP": 3}[compression]
    lines_per_block = 16 if compression == "ZIP" else 1
    header = struct.pack("<ii", EXR_MAGIC, 2)
    header += _attr("channels", "chlist", chlist)
    if chromaticities:
        header += _attr("chromaticities", "chromaticities", struct.pack("<8f", *chromaticities))
    header += _attr("compression", "compression", bytes([code]))
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header += _attr("dataWindow", "box2i", box)
    header += _attr("displayWindow", "box2i", box)
    header += _attr("lineOrder", "lineOrder", b"\0")
    header += _attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += _attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    header += _attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"
    planes = [np.ascontiguousarray(channels[name], dtype=dtype) for name in names]
    blocks = []
    for y0 in range(0, height, lines_per_block):
        y1 = min(height, y0 + lines_per_block)
        # Per scanline, each channel's row in sorted channel order.
        raw = np.stack([plane[y0:y1] for plane in planes], axis=1).tobytes()
        payload = _zip_encode(raw) if code == 3 else raw
        if code == 3 and len(payload) >= len(raw):
            payload = raw
        blocks.append(struct.pack("<ii", y0, len(payload)) + payload)
    offset = len(header) + 8 * len(blocks)
    table = bytearray()
    for block in blocks:
        table += struct.pack("<Q", offset)
        offset += len(block)
    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(table)
        for block in blocks:
            handle.write(block)


def read_exr_header(path):
    """Parse the attributes of a single-part scanline/tiled EXR (multi-part: first header)."""
    with open(path, "rb") as handle:
        data = handle.read(1 << 20)
    magic, version = struct.unpack("<ii", data[:8])
    if magic != EXR_MAGIC:
        raise ValueError(f"{path}: not an OpenEXR file")
    position = 8
    attributes = {}
    while data[position] != 0:
        end = data.index(b"\0", position)
        name = data[position:end].decode()
        kind_end = data.index(b"\0", end + 1)
        kind = data[end + 1:kind_end].decode()
        size = struct.unpack("<i", data[kind_end + 1:kind_end + 5])[0]
        value = data[kind_end + 5:kind_end + 5 + size]
        position = kind_end + 5 + size
        attributes[name] = (kind, value)
    channels = []
    raw = attributes["channels"][1]
    cursor = 0
    while raw[cursor] != 0:
        end = raw.index(b"\0", cursor)
        name = raw[cursor:end].decode()
        pixel_type = struct.unpack("<i", raw[end + 1:end + 5])[0]
        channels.append((name, EXR_PIXEL_TYPES.get(pixel_type, str(pixel_type))))
        cursor = end + 17
    x0, y0, x1, y1 = struct.unpack("<iiii", attributes["dataWindow"][1])
    return {"format": "exr", "width": x1 - x0 + 1, "height": y1 - y0 + 1,
            "compression": EXR_COMPRESSION.get(attributes["compression"][1][0], "?"),
            "channels": channels, "multipart": bool(version & 0x1000),
            "tiled": bool(version & 0x200)}


def _zip_decode(payload, expected):
    data = np.frombuffer(zlib.decompress(payload), dtype=np.uint8)
    # Undo the delta predictor: each byte is the running sum of (stored - 128), modulo 256.
    predicted = np.cumsum(np.concatenate([[data[0]], data[1:].astype(np.int64) - 128])) % 256
    half = (expected + 1) // 2
    out = np.empty(expected, dtype=np.uint8)
    out[0::2] = predicted[:half]
    out[1::2] = predicted[half:]
    return out.tobytes()


def read_exr(path):
    """Read a single-part scanline EXR with NONE, ZIPS or ZIP compression into float32 planes.

    Falls back to the OpenEXR Python module for anything else when it is installed.
    """
    header = read_exr_header(path)
    compression = header["compression"]
    if compression not in ("NONE", "ZIPS", "ZIP") or header["tiled"] or header["multipart"]:
        import OpenEXR  # optional dependency for other compressions
        with OpenEXR.File(str(path)) as exr:
            channels = exr.channels()
            planes = {}
            for name, channel in channels.items():
                pixels = np.asarray(channel.pixels, dtype=np.float32)
                if pixels.ndim == 3:
                    for index, suffix in enumerate(name):
                        planes[suffix] = pixels[..., index]
                else:
                    planes[name] = pixels
            return planes
    width, height = header["width"], header["height"]
    names = [name for name, _ in sorted(header["channels"])]
    types = dict(header["channels"])
    sizes = {name: 2 if types[name] == "HALF" else 4 for name in names}
    lines = {"NONE": 1, "ZIPS": 1, "ZIP": 16}[compression]
    blocks = (height + lines - 1) // lines
    with open(path, "rb") as handle:
        data = handle.read()
    # The header ends at the first empty attribute name after the version field.
    position = 8
    while data[position] != 0:
        end = data.index(b"\0", position)
        kind_end = data.index(b"\0", end + 1)
        size = struct.unpack("<i", data[kind_end + 1:kind_end + 5])[0]
        position = kind_end + 5 + size
    position += 1
    offsets = struct.unpack(f"<{blocks}Q", data[position:position + 8 * blocks])
    planes = {name: np.empty((height, width), dtype=np.float32) for name in names}
    row_bytes = sum(sizes.values()) * width
    for offset in offsets:
        y, size = struct.unpack("<ii", data[offset:offset + 8])
        count = min(lines, height - y)
        expected = row_bytes * count
        payload = data[offset + 8:offset + 8 + size]
        raw = payload if size >= expected else _zip_decode(payload, expected)
        cursor = 0
        for line in range(count):
            for name in names:
                n = sizes[name] * width
                planes[name][y + line] = np.frombuffer(
                    raw[cursor:cursor + n], dtype="<f2" if sizes[name] == 2 else "<f4")
                cursor += n
    return planes
