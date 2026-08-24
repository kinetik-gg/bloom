#!/usr/bin/env python3
"""Focused self-tests for Bloom's repository quality checkers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock


_ROOT = Path(__file__).resolve().parents[1]


def _load_script(name: str) -> types.ModuleType:
    path = _ROOT / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RepositoryHygieneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = _load_script("check_repository_hygiene")

    def scan(self, relative: str, content: str = "") -> list[object]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "LICENSE").write_text("Apache License\nVersion 2.0\n", encoding="utf-8")
            (root / "NOTICE").write_text("Bloom\n", encoding="utf-8")
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            return self.checker.scan_repository(
                root,
                [Path("LICENSE"), Path("NOTICE"), Path(relative)],
            )

    def test_accepts_placeholders(self) -> None:
        findings = self.scan(
            "docs/example.md",
            'password = "<secret>"\npath = "/home/<user>/project"\n',
        )
        self.assertEqual(findings, [])

    def test_rejects_absolute_account_path(self) -> None:
        findings = self.scan("docs/leak.md", "path: /home/alice/private/project\n")
        self.assertTrue(any(item.category == "machine-specific path" for item in findings))

    def test_rejects_secret_assignment(self) -> None:
        findings = self.scan("config/example.toml", 'api_key = "real-value-123"\n')
        self.assertTrue(any(item.category == "credential" for item in findings))

    def test_rejects_generated_artifact(self) -> None:
        findings = self.scan("build/output.o")
        self.assertTrue(any(item.category == "generated artifact" for item in findings))

    def test_requires_root_license_and_notice(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            findings = self.checker.scan_repository(Path(directory), [])
        categories = [item.category for item in findings]
        self.assertEqual(categories.count("license policy"), 2)

    def test_requires_vendored_license_and_inventory(self) -> None:
        findings = self.scan("third_party/example/source.cpp", "int example;\n")
        messages = [item.message for item in findings]
        self.assertTrue(any("attribution inventory" in message for message in messages))
        self.assertTrue(any("upstream license" in message for message in messages))

    def test_repository_files_skip_deleted_tracked_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "present.txt").write_text("present\n", encoding="utf-8")
            with mock.patch.object(
                self.checker,
                "_git_repository_files",
                return_value=[Path("deleted.txt"), Path("present.txt")],
            ):
                self.assertEqual(self.checker.repository_files(root), [Path("present.txt")])


class ArchitectureBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = _load_script("check_architecture_boundaries")

    def scan(self, relative: str, content: str) -> list[object]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            return self.checker.scan_repository(root, [Path(relative)])

    def test_allows_qt_in_ui(self) -> None:
        findings = self.scan("src/ui/panel.cpp", "#include <QString>\n")
        self.assertEqual(findings, [])

    def test_rejects_qt_in_document(self) -> None:
        findings = self.scan("src/document/model.cpp", "#include <QString>\n")
        self.assertTrue(any("Qt types" in item.message for item in findings))

    def test_rejects_relative_cross_boundary_include(self) -> None:
        findings = self.scan("src/document/model.cpp", '#include "../ui/panel.hpp"\n')
        self.assertTrue(any("public include roots" in item.message for item in findings))

    def test_rejects_misplaced_public_header(self) -> None:
        findings = self.scan("src/core/include/bloom/wrong.hpp", "#pragma once\n")
        self.assertTrue(any("public header" in item.message for item in findings))


if __name__ == "__main__":
    unittest.main()
