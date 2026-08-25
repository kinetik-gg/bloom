#!/usr/bin/env python3
"""Validate Bloom's checked-in project schema artifacts without third-party packages."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any, NoReturn, Sequence


_MANIFEST_SCHEMA_PATH = Path("schemas/project/manifest-1.0.schema.json")
_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"
_MANIFEST_ID = "urn:kinetik:bloom:schema:project-manifest:1.0"
_IDENTIFIER_PATTERN = r"^[a-z0-9][a-z0-9._-]{0,127}$"
_FOUNDATION_NODE_TYPES = [
    "bloom.composition-output",
    "bloom.layer-output",
    "bloom.layer-stack",
    "bloom.solid-source",
    "bloom.text-source",
]


class SchemaArtifactError(ValueError):
    """Raised when a checked-in schema artifact violates Bloom's artifact contract."""


def _fail(message: str) -> NoReturn:
    raise SchemaArtifactError(message)


def _reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _fail(f"duplicate JSON object member {key!r}")
        result[key] = value
    return result


def _reject_nonfinite(value: str) -> NoReturn:
    _fail(f"non-finite JSON number {value!r}")


def _validate_unicode_scalars(value: Any, location: str = "$") -> None:
    if isinstance(value, str):
        if any(0xD800 <= ord(character) <= 0xDFFF for character in value):
            _fail(f"{location} contains a lone escaped surrogate")
    elif isinstance(value, dict):
        for key, child in value.items():
            _validate_unicode_scalars(key, f"{location} object key")
            _validate_unicode_scalars(child, f"{location}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _validate_unicode_scalars(child, f"{location}[{index}]")


def load_strict_json(path: Path) -> Any:
    try:
        encoded = path.read_bytes()
    except OSError as error:
        _fail(f"could not read {path}: {error}")
    if encoded.startswith(b"\xef\xbb\xbf"):
        _fail(f"{path} begins with a UTF-8 BOM")
    try:
        text = encoded.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        _fail(f"{path} is not strict UTF-8: {error}")
    try:
        result = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_members,
            parse_constant=_reject_nonfinite,
        )
    except json.JSONDecodeError as error:
        _fail(f"{path} is not valid JSON: {error}")
    _validate_unicode_scalars(result)
    return result


def _require_object(value: Any, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(f"{location} must be an object")
    return value


def _json_type_strict_equal(value: Any, expected: Any) -> bool:
    if type(value) is not type(expected):
        return False
    if isinstance(value, dict):
        return value.keys() == expected.keys() and all(
            _json_type_strict_equal(value[key], expected[key]) for key in value
        )
    if isinstance(value, list):
        return len(value) == len(expected) and all(
            _json_type_strict_equal(left, right) for left, right in zip(value, expected)
        )
    return value == expected


def _require_exact(value: Any, expected: Any, location: str) -> None:
    if not _json_type_strict_equal(value, expected):
        _fail(f"{location} must equal {expected!r}")


def _resolve_local_reference(root: dict[str, Any], reference: str) -> Any:
    if not reference.startswith("#/"):
        _fail(f"schema reference {reference!r} is not repository-local")
    current: Any = root
    for encoded_component in reference[2:].split("/"):
        component = encoded_component.replace("~1", "/").replace("~0", "~")
        if not isinstance(current, dict) or component not in current:
            _fail(f"schema reference {reference!r} does not resolve")
        current = current[component]
    return current


def _validate_references(root: dict[str, Any], value: Any, location: str = "$") -> None:
    if isinstance(value, dict):
        reference = value.get("$ref")
        if reference is not None:
            if not isinstance(reference, str):
                _fail(f"{location}.$ref must be a string")
            _resolve_local_reference(root, reference)
        for key, child in value.items():
            _validate_references(root, child, f"{location}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _validate_references(root, child, f"{location}[{index}]")


def _validate_version_definition(definition: Any, location: str, *, fixed: bool) -> None:
    version = _require_object(definition, location)
    _require_exact(version.get("type"), "object", f"{location}.type")
    _require_exact(version.get("required"), ["major", "minor"], f"{location}.required")
    _require_exact(version.get("unevaluatedProperties"), True,
                   f"{location}.unevaluatedProperties")
    properties = _require_object(version.get("properties"), f"{location}.properties")
    _require_exact(set(properties), {"major", "minor"}, f"{location}.properties keys")
    if fixed:
        _require_exact(properties["major"], {"const": 1}, f"{location}.properties.major")
        _require_exact(properties["minor"], {"const": 0}, f"{location}.properties.minor")
    else:
        _require_exact(properties["major"], {"$ref": "#/$defs/positiveMajorVersion"},
                       f"{location}.properties.major")
        _require_exact(properties["minor"], {"$ref": "#/$defs/unsigned32"},
                       f"{location}.properties.minor")


def validate_manifest_schema(schema_value: Any) -> None:
    schema = _require_object(schema_value, "manifest schema root")
    _require_exact(schema.get("$schema"), _DRAFT_2020_12, "$.$schema")
    _require_exact(schema.get("$id"), _MANIFEST_ID, "$.$id")
    _require_exact(schema.get("type"), "object", "$.type")
    _require_exact(schema.get("required"),
                   ["format", "containerVersion", "document", "requirements"],
                   "$.required")
    _require_exact(schema.get("unevaluatedProperties"), True,
                   "$.unevaluatedProperties")

    properties = _require_object(schema.get("properties"), "$.properties")
    _require_exact(set(properties),
                   {"format", "containerVersion", "document", "requirements"},
                   "$.properties keys")
    _require_exact(properties["format"], {"const": "org.kinetik.bloom.project"},
                   "$.properties.format")
    _require_exact(properties["containerVersion"], {"$ref": "#/$defs/fixedVersion-1.0"},
                   "$.properties.containerVersion")
    _require_exact(properties["document"], {"$ref": "#/$defs/document-1.0"},
                   "$.properties.document")
    requirements = _require_object(properties["requirements"], "$.properties.requirements")
    _require_exact(requirements.get("type"), "array", "$.properties.requirements.type")
    _require_exact(requirements.get("maxItems"), 1_000_000,
                   "$.properties.requirements.maxItems")
    _require_exact(requirements.get("items"), {"$ref": "#/$defs/requirement-1.0"},
                   "$.properties.requirements.items")
    if "uniqueItems" in requirements:
        _fail("requirement identity uniqueness remains a Project I/O semantic check")

    definitions = _require_object(schema.get("$defs"), "$.$defs")
    expected_definitions = {
        "unsigned32",
        "positiveMajorVersion",
        "namespacedIdentifier",
        "fixedVersion-1.0",
        "requirementVersion-1.0",
        "document-1.0",
        "requirement-1.0",
    }
    _require_exact(set(definitions), expected_definitions, "$.$defs keys")
    _require_exact(definitions["unsigned32"],
                   {"type": "integer", "minimum": 0, "maximum": 4_294_967_295},
                   "$.$defs.unsigned32")
    _require_exact(definitions["positiveMajorVersion"],
                   {"type": "integer", "minimum": 1, "maximum": 4_294_967_295},
                   "$.$defs.positiveMajorVersion")

    identifier = _require_object(definitions["namespacedIdentifier"],
                                 "$.$defs.namespacedIdentifier")
    _require_exact(identifier,
                   {
                       "type": "string",
                       "minLength": 1,
                       "maxLength": 128,
                       "pattern": _IDENTIFIER_PATTERN,
                   },
                   "$.$defs.namespacedIdentifier")
    re.compile(identifier["pattern"], flags=re.ASCII)

    _validate_version_definition(definitions["fixedVersion-1.0"],
                                 "$.$defs.fixedVersion-1.0", fixed=True)
    _validate_version_definition(definitions["requirementVersion-1.0"],
                                 "$.$defs.requirementVersion-1.0", fixed=False)

    document = _require_object(definitions["document-1.0"], "$.$defs.document-1.0")
    _require_exact(document.get("type"), "object", "$.$defs.document-1.0.type")
    _require_exact(document.get("required"), ["path", "schemaVersion"],
                   "$.$defs.document-1.0.required")
    _require_exact(document.get("unevaluatedProperties"), True,
                   "$.$defs.document-1.0.unevaluatedProperties")
    document_properties = _require_object(document.get("properties"),
                                          "$.$defs.document-1.0.properties")
    _require_exact(document_properties,
                   {
                       "path": {"const": "document.json"},
                       "schemaVersion": {"$ref": "#/$defs/fixedVersion-1.0"},
                   },
                   "$.$defs.document-1.0.properties")

    requirement = _require_object(definitions["requirement-1.0"],
                                  "$.$defs.requirement-1.0")
    _require_exact(requirement.get("type"), "object", "$.$defs.requirement-1.0.type")
    _require_exact(requirement.get("required"),
                   ["providerId", "capabilityId", "schemaVersion", "providedNodeTypeIds"],
                   "$.$defs.requirement-1.0.required")
    _require_exact(requirement.get("unevaluatedProperties"), True,
                   "$.$defs.requirement-1.0.unevaluatedProperties")
    requirement_properties = _require_object(requirement.get("properties"),
                                              "$.$defs.requirement-1.0.properties")
    _require_exact(set(requirement_properties),
                   {"providerId", "capabilityId", "schemaVersion", "providedNodeTypeIds"},
                   "$.$defs.requirement-1.0.properties keys")
    for identifier_name in ("providerId", "capabilityId"):
        _require_exact(requirement_properties[identifier_name],
                       {"$ref": "#/$defs/namespacedIdentifier"},
                       f"$.$defs.requirement-1.0.properties.{identifier_name}")
    _require_exact(requirement_properties["schemaVersion"],
                   {"$ref": "#/$defs/requirementVersion-1.0"},
                   "$.$defs.requirement-1.0.properties.schemaVersion")

    provided = _require_object(requirement_properties["providedNodeTypeIds"],
                               "$.$defs.requirement-1.0.properties.providedNodeTypeIds")
    _require_exact(provided.get("type"), "array", "providedNodeTypeIds.type")
    _require_exact(provided.get("maxItems"), 1_000_000, "providedNodeTypeIds.maxItems")
    if "uniqueItems" in provided:
        _fail("provided-node ordering and uniqueness remain Project I/O semantic checks")
    provided_items = _require_object(provided.get("items"), "providedNodeTypeIds.items")
    all_of = provided_items.get("allOf")
    if not isinstance(all_of, list) or len(all_of) != 2:
        _fail("providedNodeTypeIds.items.allOf must contain two constraints")
    _require_exact(all_of[0], {"$ref": "#/$defs/namespacedIdentifier"},
                   "providedNodeTypeIds.items.allOf[0]")
    _require_exact(all_of[1], {"not": {"enum": _FOUNDATION_NODE_TYPES}},
                   "providedNodeTypeIds.items.allOf[1]")

    _validate_references(schema, schema)


def check_repository(root: Path) -> None:
    schema_path = root.resolve() / _MANIFEST_SCHEMA_PATH
    validate_manifest_schema(load_strict_json(schema_path))


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        check_repository(args.root)
    except SchemaArtifactError as error:
        print(f"Project schema check failed: {error}", file=sys.stderr)
        return 1
    print("Project schema check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
