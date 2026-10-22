#!/usr/bin/env python3
"""Bounded, local-only Bloom sprint validation with reviewable evidence."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
MAX_JOBS = 8
TARGET_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_.+\-]*$")
FOCUSED_FORBIDDEN_TARGETS = {"all", "clean", "help", "install", "package", "test", "rebuild_cache"}


class ValidationError(Exception):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise ValidationError("Git source identity failed: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def source_identity(root: Path) -> dict:
    """Hash tracked changes and contents of every non-ignored untracked file."""
    head = git(root, "rev-parse", "HEAD").decode().strip()
    branch = git(root, "branch", "--show-current").decode().strip() or None
    tracked = git(root, "diff", "HEAD", "--binary", "--no-ext-diff")
    names = [name for name in git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if name]
    digest = hashlib.sha256()
    digest.update(b"bloom-local-validation-diff-v1\0")
    digest.update(len(tracked).to_bytes(8, "big"))
    digest.update(tracked)
    for raw_name in sorted(names):
        path = root / os.fsdecode(raw_name)
        digest.update(b"untracked\0" + raw_name + b"\0")
        if path.is_symlink():
            digest.update(b"symlink\0" + os.fsencode(os.readlink(path)))
        elif path.is_file():
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
        else:
            digest.update(b"missing-or-special\0")
    return {"head": head, "branch": branch, "dirty": bool(tracked or names),
            "diffSha256": digest.hexdigest(), "untrackedFiles": len(names)}


def common_git_lock(root: Path) -> Path:
    common = Path(os.fsdecode(git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")).strip())
    return common / "bloom-local-validation.lock"


@contextmanager
def exclusive_lock(path: Path, timeout_seconds: float):
    """One advisory lock per shared Git repository, or a caller-selected machine lock."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        handle.seek(0)
        if handle.read(1) == b"":
            handle.seek(0)
            handle.write(b"\0")
            handle.flush()
        start = time.monotonic()
        while True:
            try:
                if os.name == "nt":
                    import msvcrt
                    handle.seek(0)
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() - start >= timeout_seconds:
                    raise ValidationError(f"Validation lock is busy: {path}")
                time.sleep(min(0.2, timeout_seconds))
        try:
            yield time.monotonic() - start
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def cache_values(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise ValidationError(f"No CMakeCache.txt in {build_dir}; use an explicit milestone --configure")
    values = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]*=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def check_cache(build_dir: Path, root: Path, level: str | None, prefix: Path | None,
                config: str) -> dict[str, str]:
    values = cache_values(build_dir)
    home = values.get("CMAKE_HOME_DIRECTORY")
    if not home or Path(home).resolve() != root.resolve():
        raise ValidationError("CMake build directory belongs to a different source checkout")
    if values.get("BUILD_TESTING", "").upper() not in ("ON", "TRUE", "1"):
        raise ValidationError("CMake build directory must have BUILD_TESTING=ON")
    generator = values.get("CMAKE_GENERATOR", "")
    multi_config = ("Multi-Config" in generator or "Visual Studio" in generator or
                    generator == "Xcode")
    if multi_config:
        available = values.get("CMAKE_CONFIGURATION_TYPES")
        if not available or config.lower() not in {name.lower() for name in available.split(";")}:
            raise ValidationError(f"Requested --config {config} is absent from CMAKE_CONFIGURATION_TYPES")
    elif values.get("CMAKE_BUILD_TYPE", "").lower() != config.lower():
        raise ValidationError(f"Single-config CMAKE_BUILD_TYPE does not match --config {config}")
    configured_prefix = values.get("BLOOM_DEPENDENCY_PREFIX")
    if prefix and (not configured_prefix or Path(configured_prefix).resolve() != prefix.resolve()):
        raise ValidationError("Configured dependency prefix differs from --dependency-prefix")
    if values.get("BLOOM_DEPENDENCY_MODE") == "qualified" and (
        not configured_prefix or not Path(configured_prefix).is_dir()
    ):
        raise ValidationError("Qualified dependency prefix is missing")
    for language in ("C", "CXX"):
        launcher = values.get(f"CMAKE_{language}_COMPILER_LAUNCHER", "")
        if launcher and Path(launcher).name not in ("ccache", "ccache.exe"):
            raise ValidationError("Configured compiler launcher is not local ccache")
        compiler = values.get(f"CMAKE_{language}_COMPILER", "")
        if Path(compiler).name in ("distcc", "icecc", "sccache"):
            raise ValidationError("Configured compiler is a remote-capable wrapper")
    if level == "desktop" and values.get("BLOOM_BUILD_DESKTOP", "").upper() not in ("ON", "TRUE", "1"):
        raise ValidationError("Desktop milestone requires BLOOM_BUILD_DESKTOP=ON")
    if level == "native" and values.get("BLOOM_RENDER_ENABLE_VULKAN", "").upper() not in ("ON", "TRUE", "1"):
        raise ValidationError("Native milestone requires BLOOM_RENDER_ENABLE_VULKAN=ON")
    return values


def configure_command(args: argparse.Namespace, root: Path, build_dir: Path,
                      prefix: Path | None, ccache: str | None) -> list[str]:
    if args.dependency_mode == "qualified" and (prefix is None or not prefix.is_dir()):
        raise ValidationError("Qualified configure requires an existing --dependency-prefix")
    if args.generator == "Ninja" and not shutil.which("ninja"):
        raise ValidationError("Ninja generator requested but ninja is unavailable")
    command = ["cmake", "-S", str(root), "-B", str(build_dir), "-G", args.generator,
               "-DBUILD_TESTING=ON", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
               "-DBLOOM_DEPENDENCY_MODE=" + args.dependency_mode,
               "-DBLOOM_BUILD_DESKTOP=" + ("ON" if args.level == "desktop" else "OFF"),
               "-DBLOOM_ENABLE_CLANG_TIDY=OFF", "-DBLOOM_ENABLE_TEST_CLANG_TIDY=OFF"]
    if not ("Multi-Config" in args.generator or "Visual Studio" in args.generator or
            args.generator == "Xcode"):
        command.append("-DCMAKE_BUILD_TYPE=" + args.config)
    if args.level == "native":
        command.append("-DBLOOM_RENDER_ENABLE_VULKAN=ON")
    if prefix:
        command += ["-DBLOOM_DEPENDENCY_PREFIX=" + str(prefix), "-DCMAKE_PREFIX_PATH=" + str(prefix)]
    if args.python_executable:
        command.append("-DPython3_EXECUTABLE=" + str(Path(args.python_executable).resolve()))
    if ccache:
        command += ["-DCMAKE_C_COMPILER_LAUNCHER=" + ccache,
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=" + ccache]
    return command


def local_environment(jobs: int, ccache_dir: Path | None) -> dict[str, str]:
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_QPA_PLATFORMTHEME"] = ""
    env["QT_STYLE_OVERRIDE"] = "Fusion"
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(jobs)
    env["CTEST_PARALLEL_LEVEL"] = str(jobs)
    # A user's ccache may be configured for remote storage; this runner uses local storage only.
    env["CCACHE_REMOTE_STORAGE"] = ""
    env["CCACHE_PREFIX"] = ""
    env["CCACHE_PREFIX_CPP"] = ""
    if ccache_dir:
        ccache_dir.mkdir(parents=True, exist_ok=True)
        env["CCACHE_DIR"] = str(ccache_dir)
    return env


def terminate_process_tree(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        os.killpg(process.pid, signal.SIGKILL)
    process.wait()


def execute(report: dict, argv: list[str], cwd: Path, env: dict[str, str],
            log_path: Path, timeout_seconds: int) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    step = {"argv": argv, "cwd": str(cwd), "log": str(log_path),
            "startedAt": utc_now(), "exitCode": None, "durationSeconds": None, "timedOut": False}
    report["commands"].append(step)
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log,
                                       stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
                                       start_new_session=os.name != "nt")
            try:
                step["exitCode"] = process.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                step["timedOut"] = True
                terminate_process_tree(process)
                step["exitCode"] = 124
            except KeyboardInterrupt:
                terminate_process_tree(process)
                step["exitCode"] = 130
                raise
    except OSError as error:
        step["error"] = str(error)
        step["exitCode"] = 127
    finally:
        step["durationSeconds"] = round(time.monotonic() - started, 3)
    return step["exitCode"]


def parse_junit(path: Path, expected_names: set[str] | None) -> dict:
    if not path.is_file():
        raise ValidationError(f"CTest did not produce JUnit evidence: {path}")
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise ValidationError(f"Invalid CTest JUnit evidence: {error}") from error
    cases = root.findall(".//testcase")
    names = {case.attrib.get("name", "") for case in cases}
    if not cases or (expected_names is not None and names != expected_names):
        raise ValidationError(f"CTest case mismatch: expected {sorted(expected_names or [])}, got {sorted(names)}")
    skipped = sorted(case.attrib.get("name", "") for case in cases
                     if case.find("skipped") is not None or case.attrib.get("status") == "notrun")
    failed = sorted(case.attrib.get("name", "") for case in cases
                    if case.find("failure") is not None or case.find("error") is not None)
    return {"count": len(cases), "skipped": skipped, "failed": failed,
            "status": "Passed" if not skipped and not failed else "Failed", "junit": str(path)}


def test_inventory(report: dict, build_dir: Path, config: str, env: dict[str, str], logs: Path) -> set[str]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1"]
    log = logs / "inventory.json"
    if execute(report, command, ROOT, env, log, 60):
        raise ValidationError("CTest inventory failed")
    try:
        return {entry["name"] for entry in json.loads(log.read_text(encoding="utf-8"))["tests"]}
    except (ValueError, KeyError, TypeError) as error:
        raise ValidationError(f"Invalid CTest inventory: {error}") from error


def ctest_command(build_dir: Path, config: str, jobs: int, junit: Path,
                  exact_names: list[str] | None) -> list[str]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config,
               "--output-on-failure", "--no-tests=error", "--parallel", str(jobs),
               "--output-junit", str(junit)]
    if exact_names:
        command += ["-R", "^(" + "|".join(re.escape(name) for name in exact_names) + ")$"]
    return command


def run_named(args: argparse.Namespace, report: dict, env: dict[str, str], logs: Path,
              build_dir: Path, target_limit: int, test_limit: int) -> None:
    if not args.target and not args.test:
        raise ValidationError("Named run requires at least one --target or --test")
    if len(args.target) > target_limit or len(args.test) > test_limit:
        raise ValidationError(f"Named run exceeds {target_limit} targets or {test_limit} tests")
    for target in args.target:
        if not TARGET_NAME.fullmatch(target) or target.lower() in FOCUSED_FORBIDDEN_TARGETS:
            raise ValidationError(f"Invalid or broad named target: {target}")
    if args.target:
        command = ["cmake", "--build", str(build_dir), "--config", args.config,
                   "--parallel", str(args.jobs), "--target", *dict.fromkeys(args.target)]
        if execute(report, command, ROOT, env, logs / "build.log", args.build_timeout_seconds):
            raise ValidationError("Named target build failed")
    if args.test:
        inventory = test_inventory(report, build_dir, args.config, env, logs)
        missing = sorted(set(args.test) - inventory)
        if missing:
            raise ValidationError("Requested CTest name is absent: " + ", ".join(missing))
        names = list(dict.fromkeys(args.test))
        junit = logs / "named-tests.xml"
        command = ctest_command(build_dir, args.config, args.jobs, junit, names)
        code = execute(report, command, ROOT, env, logs / "named-tests.log",
                       args.test_timeout_seconds)
        result = parse_junit(junit, set(names))
        report["tests"].append({"names": names, **result})
        if code or result["status"] != "Passed":
            raise ValidationError("Named tests failed or were skipped")


def run_milestone(args: argparse.Namespace, report: dict, env: dict[str, str], logs: Path,
                  build_dir: Path) -> None:
    if args.level == "native":
        evidence = logs / "native-gpu.json"
        command = [sys.executable, str(ROOT / "tools/ci/gpu_qualification.py"),
                   "--build-dir", str(build_dir), "--mode", "native", "--build",
                   "--require-device", "--jobs", str(args.jobs), "--config", args.config,
                   "--output", str(evidence)]
        code = execute(report, command, ROOT, env, logs / "native-gpu.log",
                       args.build_timeout_seconds + args.test_timeout_seconds)
        if evidence.is_file():
            try:
                native = json.loads(evidence.read_text(encoding="utf-8"))
            except ValueError as error:
                raise ValidationError(f"Invalid native GPU evidence: {error}") from error
            report["native"] = {"status": native.get("native", {}).get("status", "NotRun"),
                                "evidence": str(evidence),
                                "qualifiedNative": native.get("qualifiedNative", {}).get("status", "NotRun")}
        if code or report["native"]["status"] != "Passed":
            raise ValidationError("Native GPU milestone did not execute and pass on hardware")
        return

    if not args.all:
        run_named(args, report, env, logs, build_dir, 100, 200)
        return

    command = ["cmake", "--build", str(build_dir), "--config", args.config,
               "--parallel", str(args.jobs)]
    if execute(report, command, ROOT, env, logs / "build.log", args.build_timeout_seconds):
        raise ValidationError("Milestone build failed")
    junit = logs / "all-tests.xml"
    command = ctest_command(build_dir, args.config, args.jobs, junit, None)
    code = execute(report, command, ROOT, env, logs / "all-tests.log", args.test_timeout_seconds)
    result = parse_junit(junit, None)
    report["tests"].append(result)
    if code or result["status"] != "Passed":
        raise ValidationError("Milestone CTest suite failed or contains skipped tests")


def atomic_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def run(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir).expanduser().resolve()
    prefix = Path(args.dependency_prefix).expanduser().resolve() if args.dependency_prefix else None
    ccache_dir = Path(args.ccache_dir).expanduser().resolve() if args.ccache_dir else None
    output = (Path(args.output).expanduser().resolve() if args.output else
              build_dir / "local-validation" / (utc_now().replace(":", "-") + "-" + uuid.uuid4().hex[:8] + ".json"))
    logs = output.with_name(output.stem + "-logs")
    started = time.monotonic()
    report = {"schema": "bloom.local-validation.v1", "status": "NotRun",
              "mode": args.mode, "level": getattr(args, "level", None),
              "startedAt": utc_now(), "sourceRoot": str(ROOT),
              "buildDirectory": str(build_dir), "jobs": args.jobs,
              "source": None, "sourceAfter": None,
              "commands": [], "tests": [], "native": {"status": "NotRun"}}
    try:
        report["source"] = source_identity(ROOT)
        lock = Path(args.lock_file).expanduser().resolve() if args.lock_file else common_git_lock(ROOT)
        report["lockFile"] = str(lock)
        with exclusive_lock(lock, args.lock_timeout_seconds) as wait_seconds:
            report["lockWaitSeconds"] = round(wait_seconds, 3)
            if args.mode == "milestone" and args.configure:
                if (build_dir / "CMakeCache.txt").exists():
                    check_cache(build_dir, ROOT, None, prefix, args.config)
                ccache = args.ccache or shutil.which("ccache")
                if not ccache and not args.no_ccache:
                    raise ValidationError("ccache unavailable; install it or explicitly pass --no-ccache")
                if args.no_ccache:
                    ccache = None
                if ccache and Path(ccache).name not in ("ccache", "ccache.exe"):
                    raise ValidationError("--ccache must name the local ccache executable")
                command = configure_command(args, ROOT, build_dir, prefix, ccache)
                env = local_environment(args.jobs, ccache_dir)
                if execute(report, command, ROOT, env, logs / "configure.log", args.configure_timeout_seconds):
                    raise ValidationError("Explicit milestone configure failed")
            cache = check_cache(build_dir, ROOT, getattr(args, "level", None), prefix, args.config)
            if args.mode == "milestone" and cache.get("BLOOM_DEPENDENCY_MODE") != args.dependency_mode:
                raise ValidationError("Configured dependency mode differs from --dependency-mode")
            report["build"] = {"generator": cache.get("CMAKE_GENERATOR"),
                               "configuration": args.config,
                               "dependencyMode": cache.get("BLOOM_DEPENDENCY_MODE", "Unknown"),
                               "dependencyPrefix": cache.get("BLOOM_DEPENDENCY_PREFIX"),
                               "ccacheLauncher": cache.get("CMAKE_CXX_COMPILER_LAUNCHER")}
            report["qualification"] = ("Qualified-pending-manifest"
                                       if cache.get("BLOOM_DEPENDENCY_MODE") == "qualified"
                                       else "Unqualified")
            env = local_environment(args.jobs, ccache_dir)
            if args.mode == "focused":
                run_named(args, report, env, logs, build_dir, 20, 40)
            else:
                run_milestone(args, report, env, logs, build_dir)
            report["status"] = "Passed"
    except (ValidationError, KeyboardInterrupt, OSError) as error:
        report["status"] = "Failed" if report["commands"] else "NotRun"
        report["error"] = str(error) if str(error) else type(error).__name__
    finally:
        try:
            report["sourceAfter"] = source_identity(ROOT)
            if report["source"] and report["sourceAfter"] != report["source"]:
                report["status"] = "Failed"
                report["error"] = "Source changed during validation; evidence is stale"
        except ValidationError as error:
            report["status"] = "Failed"
            report["error"] = str(error)
        report["finishedAt"] = utc_now()
        report["durationSeconds"] = round(time.monotonic() - started, 3)
        atomic_json(output, report)
    print(f"{report['status']}: {output}")
    if report.get("error"):
        print(report["error"], file=sys.stderr)
    return 0 if report["status"] == "Passed" else 1


def parser() -> argparse.ArgumentParser:
    command = argparse.ArgumentParser(description=__doc__)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--build-dir", required=True, help="CMake build directory for this source checkout")
    common.add_argument("--output", help="JSON evidence path (default: unique file below build directory)")
    common.add_argument("--jobs", type=int, default=int(os.environ.get("BLOOM_LOCAL_JOBS", "2")),
                        help="Parallel build/test jobs, 1..8 (default: BLOOM_LOCAL_JOBS or 2)")
    common.add_argument("--config", default="RelWithDebInfo", help="CMake configuration")
    common.add_argument("--dependency-prefix", default=os.environ.get("BLOOM_LOCAL_DEPENDENCY_PREFIX"))
    common.add_argument("--ccache-dir", default=os.environ.get("BLOOM_LOCAL_CCACHE_DIR"))
    common.add_argument("--lock-file", default=os.environ.get("BLOOM_LOCAL_LOCK_FILE"),
                        help="Shared lock path; default is the repository's common Git directory")
    common.add_argument("--lock-timeout-seconds", type=float, default=0,
                        help="Wait at most this long for the shared lock (default: fail fast)")
    common.add_argument("--build-timeout-seconds", type=int, default=3600)
    common.add_argument("--test-timeout-seconds", type=int, default=1800)
    modes = command.add_subparsers(dest="mode", required=True)
    focused = modes.add_parser("focused", parents=[common], help="Build selected targets and/or run exact CTest names")
    focused.add_argument("--target", action="append", default=[], help="Repeat for each narrow CMake target")
    focused.add_argument("--test", action="append", default=[], help="Repeat for each exact CTest name")
    milestone = modes.add_parser("milestone", parents=[common], help="Explicit named union, full suite or native gate")
    milestone.add_argument("--level", required=True, choices=("core", "desktop", "native"))
    milestone.add_argument("--target", action="append", default=[], help="Repeat for each owning CMake target")
    milestone.add_argument("--test", action="append", default=[], help="Repeat for each exact CTest name")
    milestone.add_argument("--all", action="store_true", help="Explicitly build all targets and run all CTest cases")
    milestone.add_argument("--configure", action="store_true", help="Explicitly configure this build directory")
    milestone.add_argument("--dependency-mode", choices=("qualified", "developer-system"), default="qualified")
    milestone.add_argument("--generator", default="Ninja")
    milestone.add_argument("--python-executable", default=os.environ.get("BLOOM_LOCAL_PYTHON"),
                           help="Interpreter matching the qualified dependency prefix ABI")
    milestone.add_argument("--ccache", default=os.environ.get("BLOOM_LOCAL_CCACHE"))
    milestone.add_argument("--no-ccache", action="store_true")
    milestone.add_argument("--configure-timeout-seconds", type=int, default=600)
    return command


def main(argv: list[str] | None = None) -> int:
    command = parser()
    args = command.parse_args(argv)
    if not 1 <= args.jobs <= MAX_JOBS:
        command.error(f"--jobs must be between 1 and {MAX_JOBS}")
    if args.lock_timeout_seconds < 0 or any(value <= 0 for value in
        (args.build_timeout_seconds, args.test_timeout_seconds)):
        command.error("timeouts must be positive, and lock timeout must be non-negative")
    if args.mode == "milestone" and args.configure_timeout_seconds <= 0:
        command.error("configure timeout must be positive")
    if args.mode == "milestone" and args.no_ccache and args.ccache:
        command.error("--ccache and --no-ccache are mutually exclusive")
    if args.mode == "milestone" and args.level == "native" and (args.all or args.target or args.test):
        command.error("native milestone uses the defined fail-closed GPU gate; omit --all/--target/--test")
    if args.mode == "milestone" and args.all and (args.target or args.test):
        command.error("--all cannot be combined with named --target or --test")
    if (args.mode == "milestone" and args.level != "native" and not args.all and
        not args.target and not args.test):
        command.error("core/desktop milestone requires named --target/--test or explicit --all")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
