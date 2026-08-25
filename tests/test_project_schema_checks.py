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
        cls.document_schema_path = (
            _ROOT / "schemas" / "project" / "document-1.0.schema.json"
        )
        cls.document_schema = cls.checker.load_strict_json(cls.document_schema_path)

    def test_checked_in_manifest_schema_passes(self) -> None:
        self.checker.validate_manifest_schema(self.schema)

    def test_checked_in_document_schema_passes(self) -> None:
        self.checker.validate_document_schema(self.document_schema)

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

        unresolved_document = json.loads(json.dumps(self.document_schema))
        unresolved_document["properties"]["project"]["$ref"] = "#/$defs/missing"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unresolved_document)

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

        boolean_dimension = json.loads(json.dumps(self.document_schema))
        boolean_dimension["$defs"]["compositionFormat-1.0"]["properties"]["width"][
            "minimum"
        ] = True
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(boolean_dimension)

    def test_document_requires_normative_color_definitions_and_fixed_identity(self) -> None:
        missing_definition = json.loads(json.dumps(self.document_schema))
        del missing_definition["$defs"]["ocioContextVariable-1.0"]
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(missing_definition)

        changed_process_space = json.loads(json.dumps(self.document_schema))
        changed_process_space["$defs"]["colorSettings-1.0"]["properties"][
            "processColorSpaceId"
        ]["const"] = "scene_linear"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(changed_process_space)

    def test_document_keeps_drivers_outside_writable_sources_but_retains_watermark(self) -> None:
        driver_source = json.loads(json.dumps(self.document_schema))
        driver_source["$defs"]["parameterSource-1.0"]["oneOf"].append(
            {
                "type": "object",
                "required": ["kind", "driverId"],
                "properties": {
                    "kind": {"const": "driver-binding"},
                    "driverId": {"$ref": "#/$defs/objectId"},
                },
                "unevaluatedProperties": True,
            }
        )
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(driver_source)

        missing_watermark = json.loads(json.dumps(self.document_schema))
        highest = missing_watermark["$defs"]["highestIssued-1.0"]
        highest["required"].remove("driverBinding")
        del highest["properties"]["driverBinding"]
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(missing_watermark)

    def test_document_preserves_unknown_allocator_members_without_known_semantics(self) -> None:
        closed_allocator = json.loads(json.dumps(self.document_schema))
        closed_allocator["$defs"]["highestIssued-1.0"]["unevaluatedProperties"] = False
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(closed_allocator)

        added_known_watermark = json.loads(json.dumps(self.document_schema))
        highest = added_known_watermark["$defs"]["highestIssued-1.0"]
        highest["required"].append("futureNamespace")
        highest["properties"]["futureNamespace"] = {
            "$ref": "#/$defs/allocatorHighWater"
        }
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(added_known_watermark)

    def test_document_rejects_weakened_decimal_and_collection_bounds(self) -> None:
        weakened_id = json.loads(json.dumps(self.document_schema))
        weakened_id["$defs"]["objectId"]["pattern"] = "^[1-9][0-9]*$"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(weakened_id)

        empty_curve = json.loads(json.dumps(self.document_schema))
        empty_curve["$defs"]["animationCurve-1.0"]["oneOf"][0]["properties"][
            "keyframes"
        ]["minItems"] = 0
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(empty_curve)

    def test_document_does_not_misstate_utf8_byte_limits_as_scalar_lengths(self) -> None:
        scalar_limit = json.loads(json.dumps(self.document_schema))
        scalar_limit["$defs"]["structuralText"]["maxLength"] = 256
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(scalar_limit)

        context_limit = json.loads(json.dumps(self.document_schema))
        context_limit["$defs"]["ocioContextVariable-1.0"]["properties"]["value"][
            "maxLength"
        ] = 4096
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(context_limit)

    def test_document_discriminator_sets_remain_closed(self) -> None:
        changed_destination = json.loads(json.dumps(self.document_schema))
        changed_destination["$defs"]["inputPortReference-1.0"]["oneOf"][0][
            "properties"
        ]["kind"]["const"] = "arbitrary-input"
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(changed_destination)

        driver_subject = json.loads(json.dumps(self.document_schema))
        driver_subject["$defs"]["extensionTarget-1.0"]["properties"]["kind"][
            "enum"
        ].append("driver-binding")
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(driver_subject)

    def test_document_rejects_unexpected_root_keywords(self) -> None:
        extra_keyword = json.loads(json.dumps(self.document_schema))
        extra_keyword["allOf"] = [False]
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(extra_keyword)

    def test_document_pins_locator_and_context_lexical_constraints(self) -> None:
        unconstrained_project_path = json.loads(json.dumps(self.document_schema))
        unconstrained_project_path["$defs"]["ocioConfigLocator-1.0"]["oneOf"][1][
            "properties"
        ]["path"] = {}
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unconstrained_project_path)

        unconstrained_external_uri = json.loads(json.dumps(self.document_schema))
        unconstrained_external_uri["$defs"]["externalFileUri"] = {}
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unconstrained_external_uri)

        unconstrained_context_value = json.loads(json.dumps(self.document_schema))
        unconstrained_context_value["$defs"]["ocioContextVariable-1.0"]["properties"][
            "value"
        ] = {}
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unconstrained_context_value)

    def test_document_pins_previously_uncovered_animation_and_graph_fields(self) -> None:
        unconstrained_curve_id = json.loads(json.dumps(self.document_schema))
        unconstrained_curve_id["$defs"]["parameterSource-1.0"]["oneOf"][1][
            "properties"
        ]["curveId"] = {}
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unconstrained_curve_id)

        optional_vec2_members = json.loads(json.dumps(self.document_schema))
        optional_vec2_members["$defs"]["vec2Value-1.0"]["required"] = []
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(optional_vec2_members)

        unconstrained_node_parameters = json.loads(json.dumps(self.document_schema))
        unconstrained_node_parameters["$defs"]["node-1.0"]["properties"]["parameters"] = {}
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(unconstrained_node_parameters)

    def test_document_reports_typed_errors_for_malformed_comments_and_definitions(self) -> None:
        for definition_name in ("humanFacingName", "structuralText"):
            with self.subTest(definition=definition_name):
                boolean_comment = json.loads(json.dumps(self.document_schema))
                boolean_comment["$defs"][definition_name]["$comment"] = True
                with self.assertRaises(self.checker.SchemaArtifactError):
                    self.checker.validate_document_schema(boolean_comment)

        boolean_base64 = json.loads(json.dumps(self.document_schema))
        boolean_base64["$defs"]["canonicalBase64"] = True
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(boolean_base64)

        boolean_base64_comment = json.loads(json.dumps(self.document_schema))
        boolean_base64_comment["$defs"]["canonicalBase64"]["$comment"] = True
        with self.assertRaises(self.checker.SchemaArtifactError):
            self.checker.validate_document_schema(boolean_base64_comment)


if __name__ == "__main__":
    unittest.main()
