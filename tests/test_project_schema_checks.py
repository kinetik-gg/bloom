#!/usr/bin/env python3
"""Self-tests for Bloom's dependency-free project schema artifact checker."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import types
import unittest


_ROOT = Path(__file__).resolve().parents[1]


def _load_checker() -> types.ModuleType:
    path = _ROOT / "scripts" / "check_project_schemas.py"
    spec = importlib.util.spec_from_file_location("check_project_schemas", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ProjectSchemaCheckerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = _load_checker()
        cls.schema_path = _ROOT / "schemas" / "project" / "manifest-1.0.schema.json"
        cls.schema = cls.checker.load_strict_json(cls.schema_path)

    def test_checked_in_manifest_schema_passes(self) -> None:
        self.checker.validate_manifest_schema(self.schema)

    def test_rejects_duplicate_json_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"type":"object","type":"array"}\n', encoding="utf-8")
            with self.assertRaises(self.checker.SchemaArtifactError):
                self.checker.load_strict_json(path)

    def test_rejects_bom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bom.json"
            path.write_bytes(b"\xef\xbb\xbf{}\n")
            with self.assertRaises(self.checker.SchemaArtifactError):
                self.checker.load_strict_json(path)

    def test_rejects_lone_escaped_surrogate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "surrogate.json"
            path.write_text('{"title":"\\ud800"}\n', encoding="utf-8")
            with self.assertRaises(self.checker.SchemaArtifactError):
                self.checker.load_strict_json(path)

    def test_rejects_remote_or_unresolved_references(self) -> None:
        remote = json.loads(json.dumps(self.schema))
        remote["properties"]["document"]["$ref"] = "https://example.invalid/schema.json"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_manifest_schema(remote)

        unresolved = json.loads(json.dumps(self.schema))
        unresolved["properties"]["document"]["$ref"] = "#/$defs/missing"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_manifest_schema(unresolved)

    def test_rejects_weakened_identifier_or_resource_bounds(self) -> None:
        weakened_identifier = json.loads(json.dumps(self.schema))
        weakened_identifier["$defs"]["namespacedIdentifier"]["maxLength"] = 129
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_manifest_schema(weakened_identifier)

        weakened_count = json.loads(json.dumps(self.schema))
        weakened_count["properties"]["requirements"]["maxItems"] = 1_000_001
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_manifest_schema(weakened_count)

    def test_json_exactness_distinguishes_boolean_from_integer(self) -> None:
        boolean_version = json.loads(json.dumps(self.schema))
        boolean_version["$defs"]["fixedVersion-1.0"]["properties"]["major"]["const"] = True
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_manifest_schema(boolean_version)


if __name__ == "__main__":
    unittest.main()
