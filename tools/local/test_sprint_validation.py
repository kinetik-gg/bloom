"""Small, toolchain-free checks for the local validation runner."""

from __future__ import annotations

from argparse import Namespace
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import sprint_validation as runner


class SprintValidationTests(unittest.TestCase):
    def test_source_identity_includes_all_worktree_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.name", "Local Test"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.email", "local@test.invalid"], check=True)
            tracked = root / "tracked.txt"
            tracked.write_text("base\n")
            subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "-qm", "test: fixture"], check=True)
            clean = runner.source_identity(root)
            self.assertFalse(clean["dirty"])
            subprocess.run(["git", "-C", str(root), "commit", "--allow-empty", "-qm", "test: new head"],
                           check=True)
            new_head = runner.source_identity(root)
            self.assertNotEqual(clean["head"], new_head["head"])
            self.assertEqual(clean["diffSha256"], new_head["diffSha256"])
            self.assertNotEqual(clean, new_head)
            tracked.write_text("changed\n")
            modified = runner.source_identity(root)
            self.assertTrue(modified["dirty"])
            self.assertNotEqual(modified["diffSha256"], clean["diffSha256"])
            subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
            staged = runner.source_identity(root)
            self.assertEqual(staged["diffSha256"], modified["diffSha256"])
            untracked = root / "new.txt"
            untracked.write_text("one\n")
            first = runner.source_identity(root)
            untracked.write_text("two\n")
            second = runner.source_identity(root)
            self.assertNotEqual(first["diffSha256"], second["diffSha256"])
            self.assertEqual(second["untrackedFiles"], 1)

    def test_lock_rejects_concurrent_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "validation.lock"
            with runner.exclusive_lock(lock, 0):
                with self.assertRaisesRegex(runner.ValidationError, "busy"):
                    with runner.exclusive_lock(lock, 0):
                        pass
            with runner.exclusive_lock(lock, 0):
                pass

    def test_build_cache_must_match_checkout_and_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            root = parent / "source"
            other = parent / "other"
            build = parent / "build"
            root.mkdir()
            other.mkdir()
            build.mkdir()
            cache = build / "CMakeCache.txt"
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={other}\nBUILD_TESTING:BOOL=ON\n"
                             "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
            with self.assertRaisesRegex(runner.ValidationError, "different source"):
                runner.check_cache(build, root, None, None, "RelWithDebInfo")
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\nBUILD_TESTING:BOOL=ON\n"
                             "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
                             "BLOOM_BUILD_DESKTOP:BOOL=OFF\nBLOOM_RENDER_ENABLE_VULKAN:BOOL=OFF\n")
            with self.assertRaisesRegex(runner.ValidationError, "BLOOM_BUILD_DESKTOP"):
                runner.check_cache(build, root, "desktop", None, "RelWithDebInfo")
            with self.assertRaisesRegex(runner.ValidationError, "BLOOM_RENDER_ENABLE_VULKAN"):
                runner.check_cache(build, root, "native", None, "RelWithDebInfo")
            with self.assertRaisesRegex(runner.ValidationError, "CMAKE_BUILD_TYPE"):
                runner.check_cache(build, root, None, None, "Debug")
            cache.write_text(cache.read_text() + "CMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\n")
            with self.assertRaisesRegex(runner.ValidationError, "launcher"):
                runner.check_cache(build, root, None, None, "RelWithDebInfo")

    def test_local_environment_isolates_desktop(self):
        with patch.dict(os.environ, {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0",
                                      "QT_QPA_PLATFORM": "xcb", "QT_QPA_PLATFORMTHEME": "system",
                                      "QT_STYLE_OVERRIDE": "Windows"}):
            env = runner.local_environment(2, None)
        self.assertNotIn("DISPLAY", env)
        self.assertNotIn("WAYLAND_DISPLAY", env)
        self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
        self.assertEqual(env["QT_QPA_PLATFORMTHEME"], "")
        self.assertEqual(env["QT_STYLE_OVERRIDE"], "Fusion")
        self.assertEqual(env["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
        self.assertEqual(env["CCACHE_REMOTE_STORAGE"], "")
        self.assertEqual(env["CCACHE_PREFIX"], "")
        self.assertEqual(env["CCACHE_PREFIX_CPP"], "")

    def test_junit_skip_is_a_failure_even_when_ctest_exits_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            python = Path(sys.executable).as_posix()
            (root / "CTestTestfile.cmake").write_text(
                f'add_test(good "{python}" "-c" "pass")\n'
                f'add_test(skip "{python}" "-c" "import sys; sys.exit(77)")\n'
                'set_tests_properties(skip PROPERTIES SKIP_RETURN_CODE 77)\n')
            junit = root / "out.xml"
            result = subprocess.run(["ctest", "--test-dir", str(root), "--output-junit", str(junit)],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 0)
            evidence = runner.parse_junit(junit, {"good", "skip"})
            self.assertEqual(evidence["status"], "Failed")
            self.assertEqual(evidence["skipped"], ["skip"])

    def test_configuration_is_offline_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix"
            prefix.mkdir()
            args = Namespace(dependency_mode="qualified", generator="Ninja", config="RelWithDebInfo",
                             level="native")
            command = runner.configure_command(args, root, root / "build", prefix, "ccache")
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", command)
            self.assertIn("-DBLOOM_RENDER_ENABLE_VULKAN=ON", command)
            self.assertIn("-DBLOOM_DEPENDENCY_PREFIX=" + str(prefix), command)
            self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", command)
            self.assertNotIn("--parallel", command)

    def test_command_failure_and_timeout_are_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = {"commands": []}
            env = runner.local_environment(1, None)
            self.assertEqual(runner.execute(
                report, [sys.executable, "-c", "raise SystemExit(7)"], root, env,
                root / "failure.log", 5), 7)
            self.assertFalse(report["commands"][-1]["timedOut"])
            self.assertEqual(runner.execute(
                report, [sys.executable, "-c", "import time; time.sleep(30)"], root, env,
                root / "timeout.log", 1), 124)
            self.assertTrue(report["commands"][-1]["timedOut"])

    def test_unavailable_native_cannot_pass_on_zero_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = Namespace(level="native", jobs=1, config="RelWithDebInfo",
                             build_timeout_seconds=5, test_timeout_seconds=5)
            report = {"native": {"status": "NotRun"}}

            def unavailable(*unused_args):
                (root / "native-gpu.json").write_text('{"native": {"status": "NotRun"}}')
                return 0

            with patch.object(runner, "execute", side_effect=unavailable):
                with self.assertRaisesRegex(runner.ValidationError, "on hardware"):
                    runner.run_milestone(args, report, {}, root, root / "build")
            self.assertEqual(report["native"]["status"], "NotRun")


if __name__ == "__main__":
    unittest.main()
