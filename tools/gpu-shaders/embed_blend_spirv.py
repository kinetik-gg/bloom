#!/usr/bin/env python3
"""Offline embed generator for the Bloom BlendV1 shader family.

This is the deterministic compile/validate/embed recipe for the hand-written `blend*.comp` kernels.
It performs no runtime shader compilation and no GPU execution: it compiles one `.comp` with the
pinned `glslangValidator`, validates the result with `spirv-val`, disassembles it with `spirv-dis`
to assert the portable variants declare no Float64/Int64 capability or type, and writes the checked-in
SPIR-V `.inc` array plus updates the manifest pins.

Usage:
    python3 embed_blend_spirv.py --comp blend_portable.comp --manifest blend_portable.manifest \
        --inc blend_portable_spirv.inc --glslang <path> --spirv-val <path> --spirv-dis <path>
    python3 embed_blend_spirv.py ... --check

`--check` verifies the existing `.inc` and manifest already match the compiled artifact instead of
writing anything, so configure-time checks and a regeneration cannot silently diverge.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")


def symbol_prefix(stem: str) -> str:
    return "k" + "".join(part.capitalize() for part in stem.split("_")) + "Spirv"


def render_inc(stem: str, comp_sha: str, spv_sha: str, words: list[int]) -> str:
    prefix = symbol_prefix(stem)
    byte_count = len(words) * 4
    lines = [
        f"// Generated from {stem}.comp; do not edit by hand.",
        "//",
        f"// Source shader: {stem}.comp",
        f"//   sha256 {comp_sha}",
        f"// SPIR-V {stem}.spv",
        f"//   sha256 {spv_sha}",
        f"//   byte count {byte_count}",
        f"//   word count {len(words)}",
        "//",
        "// SPIR-V as deterministic little-endian 32-bit words, produced by",
        "// glslangValidator --target-env vulkan1.2 -V and validated by spirv-val. No runtime shader load.",
        "",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace bloom::render::vulkan_detail {",
        "",
        f"inline constexpr std::uint32_t {prefix}ByteCount = {byte_count}U;",
        f"inline constexpr std::uint32_t {prefix}WordCount = {len(words)}U;",
        f'inline constexpr char {prefix}Digest[] = "{spv_sha}";',
        f"inline constexpr std::uint32_t {prefix}Code[{prefix}WordCount] = {{",
    ]
    for index in range(0, len(words), 4):
        chunk = ", ".join(f"0x{word:08X}u" for word in words[index : index + 4])
        lines.append(f"    {chunk},")
    lines.extend(["};", "", "} // namespace bloom::render::vulkan_detail", ""])
    return "\n".join(lines)


def update_manifest(text: str, comp_sha: str, spv_sha: str) -> str:
    text = re.sub(r'("sha256"\s*:\s*")[0-9a-f]{64}(")', rf"\g<1>{comp_sha}\g<2>", text)
    text = re.sub(r'("spirvSha256"\s*:\s*")[0-9a-f]{64}(")', rf"\g<1>{spv_sha}\g<2>", text)
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comp", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--inc", required=True, type=Path)
    parser.add_argument("--glslang", required=True)
    parser.add_argument("--spirv-val", required=True)
    parser.add_argument("--spirv-dis", required=True)
    parser.add_argument("--target-env", default="vulkan1.2")
    parser.add_argument("--forbid-64bit", action="store_true",
                        help="reject Float64/Int64 capabilities and 64-bit types (portable kernels)")
    parser.add_argument("--check", action="store_true",
                        help="verify instead of write")
    arguments = parser.parse_args()

    comp_sha = sha256(arguments.comp)
    stem = arguments.comp.stem
    with tempfile.TemporaryDirectory() as directory:
        spv = Path(directory) / f"{stem}.spv"
        asm = Path(directory) / f"{stem}.spvasm"
        run([arguments.glslang, "--target-env", arguments.target_env, "-V",
             str(arguments.comp), "-o", str(spv)])
        run([arguments.spirv_val, "--target-env", arguments.target_env, str(spv)])
        run([arguments.spirv_dis, str(spv), "-o", str(asm)])
        if arguments.forbid_64bit:
            disassembly = asm.read_text()
            forbidden = re.findall(
                r"OpCapability\s+(Float64|Int64)\b|OpTypeFloat\s+64\b|OpTypeInt\s+64\b",
                disassembly)
            if forbidden:
                raise SystemExit(f"{stem}.comp requires a forbidden 64-bit capability/type: "
                                 f"{forbidden}")
        spv_sha = sha256(spv)
        words = [int.from_bytes(spv.read_bytes()[i : i + 4], "little")
                 for i in range(0, spv.stat().st_size, 4)]

    manifest_text = arguments.manifest.read_text()
    updated_manifest = update_manifest(manifest_text, comp_sha, spv_sha)
    generated_inc = render_inc(stem, comp_sha, spv_sha, words)

    if arguments.check:
        if generated_inc != arguments.inc.read_text():
            raise SystemExit(f"{arguments.inc} is stale; regenerate it")
        if comp_sha not in manifest_text:
            raise SystemExit(f"{arguments.manifest} does not bind the {stem}.comp SHA-256")
        if spv_sha not in manifest_text:
            raise SystemExit(f"{arguments.manifest} does not bind the {stem} SPIR-V SHA-256")
        print(f"{stem}: up to date (comp {comp_sha}, spirv {spv_sha})")
        return 0

    arguments.inc.write_text(generated_inc)
    if updated_manifest != manifest_text:
        arguments.manifest.write_text(updated_manifest)
    print(f"{stem}: wrote {arguments.inc}")
    print(f'  COMP_SHA256 "{comp_sha}"')
    print(f'  SPV_SHA256  "{spv_sha}"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
