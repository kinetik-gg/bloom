#!/usr/bin/env python3
"""Focused, display-isolated CPU fallback and fail-closed native GPU evidence.

This is a qualification runner, not dependency approval. Developer-system builds remain
Unqualified. A skipped optional binary can verify fallback but can never pass native evidence.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


FALLBACK_TESTS = (
    "bloom.media.image",
    "bloom.render.gpu_processing_phase_budget",
    "bloom.render.native_image_storage",
    "bloom.render.native_buffer_storage",
    "bloom.runtime.cpu_composition_evaluator",
    "bloom.runtime.image-effects",
    "bloom.runtime.gpu_prepared_upload_cache",
    "bloom.runtime.gpu_media_scene_preparation",
    "bloom.runtime.gpu_processing_phase_policy",
    "bloom.runtime.gpu_processing_phase_preparation",
    "bloom.output.export_resource_ledger",
)
FALLBACK_TARGETS = (
    "bloom_media_image_test", "bloom_render_gpu_processing_phase_budget_test",
    "bloom_render_native_image_storage_test", "bloom_render_native_buffer_storage_test",
    "bloom_runtime_cpu_composition_evaluator_test", "bloom_runtime_image_effect_test",
    "bloom_runtime_gpu_prepared_upload_cache_test", "bloom_runtime_gpu_media_scene_preparation_test",
    "bloom_runtime_gpu_processing_phase_policy_test",
    "bloom_runtime_gpu_processing_phase_preparation_test", "bloom_output_export_resource_ledger_test",
    "bloom_render_gpu_device_test", "bloom_render_gpu_resident_display_test",
)
LINUX_SOURCE_TESTS = (
    "bloom.media.protocol", "bloom.media.worker", "bloom.media.generate-video-fixtures",
    "bloom.media.video-source-revision", "bloom.runtime.gpu_media_video_scene_preparation",
)
LINUX_SOURCE_TARGETS = (
    "bloom_media_protocol_test", "bloom_media_worker_test", "bloom_media_generate_fixtures",
    "bloom_media_video_source_revision_test", "bloom_runtime_gpu_media_video_scene_preparation_test",
)
NATIVE_TESTS = (
    "bloom.render.native_processing_buffer", "bloom.render.native_image_allocation",
    "bloom.render.gpu_display_transfer", "bloom.render.gpu_resident_display",
    "bloom.render.native_operation_buffer", "bloom.render.native_presentation_allocation",
    "bloom.render.native_ocio_image_storage", "bloom.render.gpu_queue_timing",
    "bloom.render.gpu_allocator_memory",
)
NATIVE_TARGETS = (
    "bloom_render_gpu_device_test", "bloom_render_native_processing_buffer_test",
    "bloom_render_native_image_allocation_test", "bloom_render_gpu_display_transfer_test",
    "bloom_render_gpu_resident_display_test", "bloom_render_native_operation_buffer_test",
    "bloom_render_native_presentation_allocation_test", "bloom_render_native_ocio_image_storage_test",
    "bloom_render_gpu_queue_timing_test", "bloom_render_gpu_allocator_memory_test",
)


def isolated_environment():
    env = os.environ.copy()
    # No GUI is launched by this runner. Inherited desktop bindings are removed nevertheless.
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="", QT_STYLE_OVERRIDE="Fusion")
    return env


def cache_values(directory):
    values = {}
    for line in (directory / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]*=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def invoke(command, env, log, timeout=300):
    try:
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout, check=False)
        text = result.stdout
        code = result.returncode
    except subprocess.TimeoutExpired as error:
        captured = error.stdout or ""
        text = "Timed out\n" + (captured.decode("utf-8", errors="replace")
                                 if isinstance(captured, bytes) else captured)
        code = 124
    log.write_text(text, encoding="utf-8")
    return code, text


def run(args):
    directory = Path(args.build_dir).resolve()
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    logs = output.parent / (output.stem + "-logs")
    logs.mkdir(parents=True, exist_ok=True)
    env = isolated_environment()
    cache = cache_values(directory)
    prefix = cache.get("BLOOM_DEPENDENCY_PREFIX")
    if prefix and args.mode == "native":
        library_path = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
        if sys.platform == "win32":
            env["PATH"] = str(Path(prefix) / "bin") + os.pathsep + env.get("PATH", "")
        else:
            env[library_path] = os.pathsep.join([str(Path(prefix) / "lib"),
                                               str(Path(prefix) / "lib64"),
                                               env.get(library_path, "")])
    dependency_mode = cache.get("BLOOM_DEPENDENCY_MODE", "Unknown")
    report = {
        "schema": "bloom.gpu-qualification.v1", "platform": sys.platform,
        "dependencyMode": dependency_mode,
        "dependencyProvenance": "Qualified-pending-manifest" if dependency_mode == "qualified"
                                else "Unqualified",
        "cpuFallback": {"status": "NotRun"},
        "native": {"status": "NotRun", "reason": "Native qualification was not requested"},
        "qualifiedNative": {"status": "NotRun"}, "tests": [],
    }
    failure = False
    try:
        if args.build:
            targets = (FALLBACK_TARGETS + (LINUX_SOURCE_TARGETS if sys.platform.startswith("linux") else ())
                       if args.mode == "fallback" else NATIVE_TARGETS)
            code, _ = invoke(["cmake", "--build", str(directory), "--config", args.config,
                              "--parallel", str(args.jobs), "--target", *targets], env,
                             logs / "build.log", timeout=3600)
            if code:
                raise RuntimeError("Focused build failed")
        inventory = subprocess.run(
            ["ctest", "--test-dir", str(directory), "-C", args.config, "--show-only=json-v1"],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        tests = {entry["name"]: entry for entry in json.loads(inventory.stdout)["tests"]}

        def execute(name, extra=(), label=None):
            entry = tests.get(name)
            if not entry or not entry.get("command") or not Path(entry["command"][0]).is_file():
                raise RuntimeError("Required test binary is missing: " + name)
            code, text = invoke([*entry["command"], *extra], env,
                                logs / ((label or name) + ".log"))
            return code, text

        if args.mode == "fallback":
            if cache.get("BLOOM_RENDER_ENABLE_VULKAN", "").upper() not in ("OFF", "FALSE", "0"):
                raise RuntimeError("Fallback evidence requires an explicit Vulkan-OFF build")
            for name in FALLBACK_TESTS + (LINUX_SOURCE_TESTS if sys.platform.startswith("linux") else ()):
                code, text = execute(name)
                passed = code == 0 and not re.search(r"\b(SKIP|NotRun|Unsupported:)\b", text)
                report["tests"].append({"name": name, "status": "Passed" if passed else "Failed",
                                        "exitCode": code})
                failure = failure or not passed
            optional, optional_text = execute("bloom.render.gpu_resident_display")
            required, required_text = execute("bloom.render.gpu_resident_display",
                                               ("--require-device",), "require-device")
            rejected = (optional == 0 and "SKIP:" in optional_text and
                        "built without Vulkan" in optional_text and required != 0 and
                        "required device unavailable" in required_text)
            report["native"] = {"status": "NotRun", "reason": "Vulkan explicitly disabled",
                                 "requireDeviceRejected": rejected}
            failure = failure or not rejected
            report["cpuFallback"] = {"status": "Failed" if failure else "Passed"}
        else:
            code, text = execute("bloom.render.gpu_device", ("--require-device",), "device-probe")
            match = re.search(r"device_type=([a-z-]+)", text)
            device_type = match.group(1) if match else "unknown"
            if code and "required device unavailable:" in text:
                report["native"] = {"status": "NotRun", "reason": "Compatible device unavailable",
                                     "requireDeviceRejected": True}
                failure = args.require_device
            elif code:
                raise RuntimeError("Native device probe failed an invariant")
            elif device_type not in ("discrete-gpu", "integrated-gpu", "virtual-gpu"):
                report["native"] = {"status": "NotRun", "reason": "Hardware GPU not proven",
                                     "deviceType": device_type}
                failure = args.require_device
            else:
                for name in NATIVE_TESTS:
                    code, text = execute(name, ("--require-device",))
                    passed = code == 0 and not re.search(r"\b(SKIP|NotRun|Unsupported:)\b", text)
                    report["tests"].append({"name": name,
                                            "status": "Passed" if passed else "Failed",
                                            "exitCode": code})
                    failure = failure or not passed
                report["native"] = {"status": "Failed" if failure else "Passed",
                                     "deviceType": device_type,
                                     "scope": "Bounded compute/storage/display parity; no GUI, routes or sustained claim"}
                if dependency_mode == "qualified" and not failure:
                    report["qualifiedNative"] = {
                        "status": "NotRun",
                        "reason": "Full prefix/platform/all-route qualification is outside this bounded gate"}
        if dependency_mode != "qualified":
            report["qualifiedNative"] = {
                "status": "NotRun", "reason": "No accepted toolchain/prefix qualification in this build"}
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        failure = True
        report["error"] = str(error)
        report["cpuFallback" if args.mode == "fallback" else "native"] = {"status": "Failed"}
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if failure else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--mode", choices=("fallback", "native"), required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--require-device", action="store_true")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
