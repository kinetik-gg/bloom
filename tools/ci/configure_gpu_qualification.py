#!/usr/bin/env python3
"""Configure the focused portability gate from Bloom's locked dependency recipes."""

import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("dependencies", "project"))
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--download-dir")
    parser.add_argument("--mode", choices=("fallback", "native"), default="fallback")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    prefix = Path(args.prefix).resolve()
    directory = Path(args.build_dir).resolve()
    generator = ["-G", "Visual Studio 17 2022", "-A", "x64"] if sys.platform == "win32" else ["-G", "Ninja"]
    common = ["-DCMAKE_BUILD_TYPE=Release", *generator]
    if args.stage == "dependencies":
        if not args.download_dir:
            parser.error("dependencies requires --download-dir")
        command = ["cmake", "-S", str(root / "dependencies/superbuild"), "-B", str(directory),
                   *common, "-DBLOOM_FFMPEG_ENABLE_VAAPI=" + ("ON" if sys.platform.startswith("linux") else "OFF"),
                   "-DBLOOM_DEPENDENCY_PREFIX=" + str(prefix),
                   "-DBLOOM_DEPENDENCY_DOWNLOAD_DIR=" + str(Path(args.download_dir).resolve())]
        subprocess.run(command, check=True)
        # Build exactly the image/core closure. Linux's main configure also needs its worker's
        # FFmpeg package; other platforms must never execute the Linux-only make-based recipes.
        targets = ["bloom_dependency_yyjson", "bloom_dependency_libzip",
                   "bloom_dependency_openexr", "bloom_dependency_opencolorio"]
        if sys.platform.startswith("linux"):
            targets.append("bloom_dependency_ffmpeg")
        if args.mode == "native":
            targets += ["bloom_dependency_vulkan-headers", "bloom_dependency_vulkan-memory-allocator",
                        "bloom_dependency_vulkan-loader", "bloom_dependency_glslang",
                        "bloom_dependency_spirv-tools"]
        subprocess.run(["cmake", "--build", str(directory), "--config", "Release", "--parallel",
                        str(args.jobs), "--target", *targets], check=True)
    else:
        dependency_mode = "qualified" if sys.platform.startswith("linux") else "developer-system"
        subprocess.run([
            "cmake", "-S", str(root), "-B", str(directory), *common,
            "-DBLOOM_DEPENDENCY_MODE=" + dependency_mode,
            "-DBLOOM_DEPENDENCY_PREFIX=" + str(prefix), "-DCMAKE_PREFIX_PATH=" + str(prefix),
            "-DBLOOM_BUILD_DESKTOP=OFF", "-DBLOOM_BUILD_PYTHON=OFF", "-DBUILD_TESTING=ON",
            "-DBLOOM_ENABLE_CLANG_TIDY=OFF", "-DBLOOM_ENABLE_TEST_CLANG_TIDY=OFF",
            "-DBLOOM_RENDER_ENABLE_VULKAN=" + ("OFF" if args.mode == "fallback" else "ON"),
        ], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
