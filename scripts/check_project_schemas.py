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
_DOCUMENT_SCHEMA_PATH = Path("schemas/project/document-1.0.schema.json")
_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"
_MANIFEST_ID = "urn:kinetik:bloom:schema:project-manifest:1.0"
_DOCUMENT_ID = "urn:kinetik:bloom:schema:project-document:1.0"
_IDENTIFIER_PATTERN = r"^[a-z0-9][a-z0-9._-]{0,127}$"
_OBJECT_ID_PATTERN = (
    r"^(?:[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|"
    r"184[0-3][0-9]{16}|1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|"
    r"184467[0-3][0-9]{13}|1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|"
    r"1844674407[0-2][0-9]{9}|18446744073[0-6][0-9]{8}|"
    r"1844674407370[0-8][0-9]{6}|18446744073709[0-4][0-9]{5}|"
    r"184467440737095[0-4][0-9]{4}|1844674407370955[0][0-9]{3}|"
    r"18446744073709551[0-5][0-9]{2}|184467440737095516[0][0-9]|"
    r"1844674407370955161[0-5])$"
)
_ALLOCATOR_HIGH_WATER_PATTERN = _OBJECT_ID_PATTERN.replace("^(?:", "^(?:0|")
_POSITIVE_SIGNED64_BODY = (
    r"[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|"
    r"92[0-1][0-9]{16}|922[0-2][0-9]{15}|9223[0-2][0-9]{14}|"
    r"92233[0-6][0-9]{13}|922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|"
    r"922337203[0-5][0-9]{9}|9223372036[0-7][0-9]{8}|"
    r"92233720368[0-4][0-9]{7}|922337203685[0-3][0-9]{6}|"
    r"9223372036854[0-6][0-9]{5}|92233720368547[0-6][0-9]{4}|"
    r"922337203685477[0-4][0-9]{3}|9223372036854775[0-7][0-9]{2}|"
    r"922337203685477580[0-7]"
)
_POSITIVE_SIGNED64_PATTERN = r"^(?:" + _POSITIVE_SIGNED64_BODY + r")$"
_NEGATIVE_SIGNED64_MAGNITUDE_BODY = (
    _POSITIVE_SIGNED64_BODY.rsplit("|", maxsplit=1)[0] + r"|922337203685477580[0-8]"
)
_SIGNED64_PATTERN = (
    r"^(?:0|" + _POSITIVE_SIGNED64_BODY + r"|-(?:" +
    _NEGATIVE_SIGNED64_MAGNITUDE_BODY + r"))$"
)
_POSITIVE_UNSIGNED32_DECIMAL_PATTERN = (
    r"^(?:[1-9][0-9]{0,8}|[1-3][0-9]{9}|4[0-1][0-9]{8}|42[0-8][0-9]{7}|"
    r"429[0-3][0-9]{6}|4294[0-8][0-9]{5}|42949[0-5][0-9]{4}|"
    r"429496[0-6][0-9]{3}|4294967[0-1][0-9]{2}|42949672[0-8][0-9]|"
    r"429496729[0-5])$"
)
_BASE64_PATTERN = r"^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$"
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


def _validate_version_definition(definition: Any, location: str, *, fixed: bool,
                                 major_reference: str = "#/$defs/positiveMajorVersion") -> None:
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
        _require_exact(properties["major"], {"$ref": major_reference},
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


def _validate_object_shape(definitions: dict[str, Any], name: str,
                           required: list[str], property_names: set[str]) -> dict[str, Any]:
    location = f"$.$defs.{name}"
    definition = _require_object(definitions[name], location)
    expected_keys = {"type", "required", "properties", "unevaluatedProperties"}
    if "$comment" in definition:
        expected_keys.add("$comment")
        if not isinstance(definition["$comment"], str):
            _fail(f"{location}.$comment must be a string")
    _require_exact(set(definition), expected_keys, f"{location} keys")
    _require_exact(definition["type"], "object", f"{location}.type")
    _require_exact(definition["required"], required, f"{location}.required")
    _require_exact(definition["unevaluatedProperties"], True,
                   f"{location}.unevaluatedProperties")
    properties = _require_object(definition["properties"], f"{location}.properties")
    _require_exact(set(properties), property_names, f"{location}.properties keys")
    return properties


def _validate_discriminated_union(definitions: dict[str, Any], name: str,
                                  expected: list[tuple[str, list[str], set[str]]]
                                  ) -> dict[str, dict[str, Any]]:
    location = f"$.$defs.{name}"
    definition = _require_object(definitions[name], location)
    _require_exact(set(definition), {"oneOf"}, f"{location} keys")
    alternatives = definition["oneOf"]
    if not isinstance(alternatives, list) or len(alternatives) != len(expected):
        _fail(f"{location}.oneOf must contain exactly {len(expected)} known alternatives")

    result: dict[str, dict[str, Any]] = {}
    for index, (kind, required, property_names) in enumerate(expected):
        branch_location = f"{location}.oneOf[{index}]"
        branch = _require_object(alternatives[index], branch_location)
        _require_exact(set(branch),
                       {"type", "required", "properties", "unevaluatedProperties"},
                       f"{branch_location} keys")
        _require_exact(branch["type"], "object", f"{branch_location}.type")
        _require_exact(branch["required"], required, f"{branch_location}.required")
        _require_exact(branch["unevaluatedProperties"], True,
                       f"{branch_location}.unevaluatedProperties")
        properties = _require_object(branch["properties"], f"{branch_location}.properties")
        _require_exact(set(properties), property_names,
                       f"{branch_location}.properties keys")
        _require_exact(properties["kind"], {"const": kind},
                       f"{branch_location}.properties.kind")
        result[kind] = properties
    return result


def _validate_array(value: Any, location: str, item_reference: str, *,
                    maximum: int = 1_000_000, minimum: int | None = None) -> None:
    array = _require_object(value, location)
    expected: dict[str, Any] = {
        "type": "array",
        "maxItems": maximum,
        "items": {"$ref": item_reference},
    }
    if minimum is not None:
        expected["minItems"] = minimum
    _require_exact(array, expected, location)


def validate_document_schema(schema_value: Any) -> None:
    schema = _require_object(schema_value, "document schema root")
    _require_exact(set(schema), {
        "$schema", "$id", "title", "$comment", "type", "required", "properties", "$defs",
        "unevaluatedProperties",
    }, "document schema root keys")
    _require_exact(schema.get("$schema"), _DRAFT_2020_12, "$.$schema")
    _require_exact(schema.get("$id"), _DOCUMENT_ID, "$.$id")
    _require_exact(schema.get("title"), "Bloom Project Document 1.0", "$.title")
    _require_exact(schema.get("type"), "object", "$.type")
    _require_exact(schema.get("required"),
                   ["schemaVersion", "project", "idAllocation", "extensions"],
                   "$.required")
    _require_exact(schema.get("unevaluatedProperties"), True,
                   "$.unevaluatedProperties")
    if not isinstance(schema.get("$comment"), str):
        _fail("$.$comment must name the semantic checks outside JSON Schema")

    properties = _require_object(schema.get("properties"), "$.properties")
    _require_exact(set(properties), {"schemaVersion", "project", "idAllocation", "extensions"},
                   "$.properties keys")
    _require_exact(properties["schemaVersion"], {"$ref": "#/$defs/fixedVersion-1.0"},
                   "$.properties.schemaVersion")
    _require_exact(properties["project"], {"$ref": "#/$defs/project-1.0"},
                   "$.properties.project")
    _require_exact(properties["idAllocation"], {"$ref": "#/$defs/idAllocation-1.0"},
                   "$.properties.idAllocation")
    _validate_array(properties["extensions"], "$.properties.extensions",
                    "#/$defs/extensionRecord-1.0")

    definitions = _require_object(schema.get("$defs"), "$.$defs")
    expected_definitions = {
        "unsigned32", "positiveUnsigned32", "fixedVersion-1.0", "schemaVersion-1.0",
        "objectId", "allocatorHighWater", "signed64Decimal", "positiveSigned64Decimal",
        "positiveUnsigned32Decimal", "humanFacingName", "structuralText",
        "namespacedIdentifier", "rationalTime", "positiveRationalTime", "unsignedRatio",
        "project-1.0", "composition-1.0", "compositionFormat-1.0",
        "colorSettings-1.0", "ocioConfigReference-1.0", "ocioConfigLocator-1.0",
        "externalFileUri", "ocioConfigRevision-1.0", "ocioContextVariable-1.0",
        "parameter-1.0", "parameterSource-1.0", "parameterValue-1.0",
        "animationCurve-1.0", "scalarKeyframe-1.0", "vec2Keyframe-1.0",
        "vec2Value-1.0", "graph-1.0", "node-1.0", "parameterBinding-1.0",
        "edge-1.0", "outputPortReference-1.0", "inputPortReference-1.0",
        "layerOutput-1.0", "layerStack-1.0", "layerStackEntry-1.0",
        "idAllocation-1.0", "highestIssued-1.0", "extensionRecord-1.0",
        "extensionTarget-1.0", "extensionReferencePolicy-1.0",
        "extensionHostReference-1.0", "canonicalBase64",
    }
    _require_exact(set(definitions), expected_definitions, "$.$defs keys")

    _require_exact(definitions["unsigned32"],
                   {"type": "integer", "minimum": 0, "maximum": 4_294_967_295},
                   "$.$defs.unsigned32")
    _require_exact(definitions["positiveUnsigned32"],
                   {"type": "integer", "minimum": 1, "maximum": 4_294_967_295},
                   "$.$defs.positiveUnsigned32")
    _validate_version_definition(definitions["fixedVersion-1.0"],
                                 "$.$defs.fixedVersion-1.0", fixed=True)
    _validate_version_definition(definitions["schemaVersion-1.0"],
                                 "$.$defs.schemaVersion-1.0", fixed=False,
                                 major_reference="#/$defs/positiveUnsigned32")

    exact_patterns = {
        "objectId": _OBJECT_ID_PATTERN,
        "allocatorHighWater": _ALLOCATOR_HIGH_WATER_PATTERN,
        "signed64Decimal": _SIGNED64_PATTERN,
        "positiveSigned64Decimal": _POSITIVE_SIGNED64_PATTERN,
        "positiveUnsigned32Decimal": _POSITIVE_UNSIGNED32_DECIMAL_PATTERN,
    }
    for name, pattern in exact_patterns.items():
        _require_exact(definitions[name], {"type": "string", "pattern": pattern},
                       f"$.$defs.{name}")
        re.compile(pattern, flags=re.ASCII)

    for name, maximum_label in (("humanFacingName", "4096-byte"),
                                ("structuralText", "256-byte")):
        value = _require_object(definitions[name], f"$.$defs.{name}")
        _require_exact(set(value), {"$comment", "type", "minLength"},
                       f"$.$defs.{name} keys")
        _require_exact(value["type"], "string", f"$.$defs.{name}.type")
        _require_exact(value["minLength"], 1, f"$.$defs.{name}.minLength")
        comment = value["$comment"]
        if (not isinstance(comment, str) or maximum_label not in comment or
                "maxLength" in value):
            _fail(f"$.$defs.{name} must leave its UTF-8 byte ceiling to Project I/O")
    _require_exact(definitions["namespacedIdentifier"], {
        "type": "string", "minLength": 1, "maxLength": 128,
        "pattern": _IDENTIFIER_PATTERN,
    }, "$.$defs.namespacedIdentifier")

    rational_shapes = {
        "rationalTime": ("#/$defs/signed64Decimal", "#/$defs/positiveSigned64Decimal"),
        "positiveRationalTime": ("#/$defs/positiveSigned64Decimal",
                                 "#/$defs/positiveSigned64Decimal"),
        "unsignedRatio": ("#/$defs/positiveUnsigned32Decimal",
                          "#/$defs/positiveUnsigned32Decimal"),
    }
    for name, (numerator, denominator) in rational_shapes.items():
        rational = _validate_object_shape(definitions, name,
                                          ["numerator", "denominator"],
                                          {"numerator", "denominator"})
        _require_exact(rational["numerator"], {"$ref": numerator},
                       f"$.$defs.{name}.properties.numerator")
        _require_exact(rational["denominator"], {"$ref": denominator},
                       f"$.$defs.{name}.properties.denominator")

    project = _validate_object_shape(
        definitions, "project-1.0", ["id", "name", "colorSettings", "compositions"],
        {"id", "name", "colorSettings", "compositions"})
    _require_exact(project["colorSettings"], {"$ref": "#/$defs/colorSettings-1.0"},
                   "$.$defs.project-1.0.properties.colorSettings")
    _validate_array(project["compositions"], "$.$defs.project-1.0.properties.compositions",
                    "#/$defs/composition-1.0")

    composition = _validate_object_shape(
        definitions, "composition-1.0",
        ["id", "name", "duration", "format", "parameters", "animationCurves", "graph"],
        {"id", "name", "duration", "format", "parameters", "animationCurves", "graph"})
    _require_exact(composition["duration"], {"$ref": "#/$defs/positiveRationalTime"},
                   "$.$defs.composition-1.0.properties.duration")
    _validate_array(composition["parameters"],
                    "$.$defs.composition-1.0.properties.parameters",
                    "#/$defs/parameter-1.0")
    _validate_array(composition["animationCurves"],
                    "$.$defs.composition-1.0.properties.animationCurves",
                    "#/$defs/animationCurve-1.0")

    format_properties = _validate_object_shape(
        definitions, "compositionFormat-1.0", ["width", "height", "pixelAspect", "frameRate"],
        {"width", "height", "pixelAspect", "frameRate"})
    dimension = {"type": "integer", "minimum": 1, "maximum": 1_048_576}
    _require_exact(format_properties["width"], dimension,
                   "$.$defs.compositionFormat-1.0.properties.width")
    _require_exact(format_properties["height"], dimension,
                   "$.$defs.compositionFormat-1.0.properties.height")

    color = _validate_object_shape(
        definitions, "colorSettings-1.0", ["schemaVersion", "processColorSpaceId", "ocioConfig"],
        {"schemaVersion", "processColorSpaceId", "ocioConfig"})
    _require_exact(color["schemaVersion"], {"$ref": "#/$defs/fixedVersion-1.0"},
                   "$.$defs.colorSettings-1.0.properties.schemaVersion")
    _require_exact(color["processColorSpaceId"], {"const": "lin_rec709_scene"},
                   "$.$defs.colorSettings-1.0.properties.processColorSpaceId")
    _require_exact(color["ocioConfig"], {"$ref": "#/$defs/ocioConfigReference-1.0"},
                   "$.$defs.colorSettings-1.0.properties.ocioConfig")

    ocio_reference = _validate_object_shape(
        definitions, "ocioConfigReference-1.0",
        ["schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"],
        {"schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"})
    _require_exact(ocio_reference["portability"],
                   {"enum": ["builtin", "project-relative", "external"]},
                   "$.$defs.ocioConfigReference-1.0.properties.portability")
    _validate_array(ocio_reference["contextVariables"],
                    "$.$defs.ocioConfigReference-1.0.properties.contextVariables",
                    "#/$defs/ocioContextVariable-1.0", maximum=256)

    locators = _validate_discriminated_union(definitions, "ocioConfigLocator-1.0", [
        ("builtin", ["kind", "uri"], {"kind", "uri"}),
        ("project-relative-ocioz", ["kind", "path"], {"kind", "path"}),
        ("external-ocioz", ["kind", "uri"], {"kind", "uri"}),
        ("external-config", ["kind", "uri"], {"kind", "uri"}),
    ])
    _require_exact(locators["builtin"]["uri"],
                   {"const": "bloom://ocio/neutral-v1/config.ocio"},
                   "$.$defs.ocioConfigLocator-1.0 builtin URI")
    _require_exact(locators["project-relative-ocioz"]["path"], {
        "$comment": (
            "The 4096-byte UTF-8 ceiling and complete normalized-component profile are Project "
            "I/O checks."
        ),
        "type": "string",
        "minLength": 1,
        "allOf": [
            {"pattern": r"^(?!/)(?![A-Za-z]:)(?!.*\\)(?!.*//)[^\u0000]+$"},
            {"pattern": r"\.ocioz$"},
        ],
    }, "$.$defs.ocioConfigLocator-1.0 project-relative-ocioz path")
    for kind in ("external-ocioz", "external-config"):
        _require_exact(locators[kind]["uri"], {"$ref": "#/$defs/externalFileUri"},
                       f"$.$defs.ocioConfigLocator-1.0 {kind} URI")
    _require_exact(definitions["externalFileUri"], {
        "$comment": (
            "Project I/O enforces the exact RFC 8089 subset, percent-escape rules, and decoded "
            ".ocioz/config.ocio leaf name."
        ),
        "type": "string",
        "minLength": 1,
        "maxLength": 16_384,
        "allOf": [
            {"pattern": r"^[\u0001-\u007f]+$"},
            {"pattern": "^[Ff][Ii][Ll][Ee]:"},
            {"pattern": "^[^?#]*$"},
        ],
    }, "$.$defs.externalFileUri")

    revision = _validate_object_shape(
        definitions, "ocioConfigRevision-1.0", ["algorithm", "digest"],
        {"algorithm", "digest"})
    _require_exact(revision["algorithm"], {"const": "sha256"},
                   "$.$defs.ocioConfigRevision-1.0.properties.algorithm")
    _require_exact(revision["digest"], {
        "type": "string", "minLength": 64, "maxLength": 64,
        "pattern": "^[0-9a-f]{64}$",
    }, "$.$defs.ocioConfigRevision-1.0.properties.digest")

    context = _validate_object_shape(definitions, "ocioContextVariable-1.0",
                                     ["name", "value"], {"name", "value"})
    _require_exact(context["name"], {
        "type": "string", "minLength": 1, "maxLength": 128,
        "pattern": "^[A-Za-z_][A-Za-z0-9_]{0,127}$",
    }, "$.$defs.ocioContextVariable-1.0.properties.name")
    _require_exact(context["value"], {
        "$comment": "The normative 4096-byte UTF-8 ceiling is enforced by Project I/O.",
        "type": "string",
        "pattern": r"^[^\u0000]*$",
    }, "$.$defs.ocioContextVariable-1.0.properties.value")

    _validate_object_shape(definitions, "parameter-1.0", ["id", "schemaKey", "source"],
                           {"id", "schemaKey", "source"})
    sources = _validate_discriminated_union(definitions, "parameterSource-1.0", [
        ("constant", ["kind", "value"], {"kind", "value"}),
        ("animation-curve", ["kind", "curveId"], {"kind", "curveId"}),
    ])
    _require_exact(sources["constant"]["value"], {"$ref": "#/$defs/parameterValue-1.0"},
                   "$.$defs.parameterSource-1.0 constant value")
    _require_exact(sources["animation-curve"]["curveId"], {"$ref": "#/$defs/objectId"},
                   "$.$defs.parameterSource-1.0 animation-curve curveId")
    if any(kind.startswith("driver") for kind in sources):
        _fail("DriverBindingSource is outside the writable document 1.0 source set")

    parameter_values = _validate_discriminated_union(definitions, "parameterValue-1.0", [
        ("bool", ["kind", "value"], {"kind", "value"}),
        ("int64", ["kind", "value"], {"kind", "value"}),
        ("float64", ["kind", "value"], {"kind", "value"}),
        ("vec2", ["kind", "x", "y"], {"kind", "x", "y"}),
        ("color4", ["kind", "red", "green", "blue", "alpha"],
         {"kind", "red", "green", "blue", "alpha"}),
        ("string", ["kind", "value"], {"kind", "value"}),
        ("rational", ["kind", "numerator", "denominator"],
         {"kind", "numerator", "denominator"}),
    ])
    _require_exact(parameter_values["bool"]["value"], {"type": "boolean"},
                   "$.$defs.parameterValue-1.0 bool value")
    _require_exact(parameter_values["int64"]["value"], {"$ref": "#/$defs/signed64Decimal"},
                   "$.$defs.parameterValue-1.0 int64 value")
    _require_exact(parameter_values["float64"]["value"], {"type": "number"},
                   "$.$defs.parameterValue-1.0 float64 value")

    curves = _validate_discriminated_union(definitions, "animationCurve-1.0", [
        ("scalar", ["id", "kind", "keyframes"], {"id", "kind", "keyframes"}),
        ("vec2", ["id", "kind", "keyframes"], {"id", "kind", "keyframes"}),
    ])
    _validate_array(curves["scalar"]["keyframes"], "$.$defs.animationCurve-1.0 scalar keys",
                    "#/$defs/scalarKeyframe-1.0", minimum=1)
    _validate_array(curves["vec2"]["keyframes"], "$.$defs.animationCurve-1.0 vec2 keys",
                    "#/$defs/vec2Keyframe-1.0", minimum=1)
    _validate_object_shape(definitions, "scalarKeyframe-1.0",
                           ["id", "time", "value", "outgoingInterpolation"],
                           {"id", "time", "value", "outgoingInterpolation"})
    _validate_object_shape(definitions, "vec2Keyframe-1.0",
                           ["id", "time", "value", "outgoingInterpolation"],
                           {"id", "time", "value", "outgoingInterpolation"})
    _validate_object_shape(definitions, "vec2Value-1.0", ["x", "y"], {"x", "y"})

    graph = _validate_object_shape(
        definitions, "graph-1.0", ["nodes", "edges", "layerOutputs", "layerStack",
                                   "compositionOutput"],
        {"nodes", "edges", "layerOutputs", "layerStack", "compositionOutput"})
    for member, target in (("nodes", "node-1.0"), ("edges", "edge-1.0"),
                           ("layerOutputs", "layerOutput-1.0")):
        _validate_array(graph[member], f"$.$defs.graph-1.0.properties.{member}",
                        f"#/$defs/{target}")
    node = _validate_object_shape(definitions, "node-1.0",
                                  ["id", "typeId", "schemaVersion", "parameters"],
                                  {"id", "typeId", "schemaVersion", "parameters"})
    _validate_array(node["parameters"], "$.$defs.node-1.0.properties.parameters",
                    "#/$defs/parameterBinding-1.0")
    _validate_object_shape(definitions, "parameterBinding-1.0", ["role", "parameterId"],
                           {"role", "parameterId"})
    _validate_object_shape(definitions, "edge-1.0", ["id", "source", "destination"],
                           {"id", "source", "destination"})
    _validate_object_shape(definitions, "outputPortReference-1.0", ["nodeId", "port"],
                           {"nodeId", "port"})
    _validate_discriminated_union(definitions, "inputPortReference-1.0", [
        ("node-input", ["kind", "nodeId", "port"], {"kind", "nodeId", "port"}),
        ("layer-stack-input", ["kind", "stackNodeId", "slotId", "role"],
         {"kind", "stackNodeId", "slotId", "role"}),
    ])
    _validate_object_shape(definitions, "layerOutput-1.0",
                           ["nodeId", "layerId", "name", "outputPort"],
                           {"nodeId", "layerId", "name", "outputPort"})
    layer_stack = _validate_object_shape(definitions, "layerStack-1.0", ["nodeId", "entries"],
                                         {"nodeId", "entries"})
    _validate_array(layer_stack["entries"], "$.$defs.layerStack-1.0.properties.entries",
                    "#/$defs/layerStackEntry-1.0")
    _validate_object_shape(definitions, "layerStackEntry-1.0", ["slotId", "layerId"],
                           {"slotId", "layerId"})

    _validate_object_shape(definitions, "idAllocation-1.0", ["highestIssued"],
                           {"highestIssued"})
    watermark_names = ["composition", "node", "edge", "layer", "layerSlot", "parameter",
                       "animationCurve", "keyframe", "driverBinding", "extensionRecord"]
    watermarks = _validate_object_shape(definitions, "highestIssued-1.0", watermark_names,
                                        set(watermark_names))
    for name in watermark_names:
        _require_exact(watermarks[name], {"$ref": "#/$defs/allocatorHighWater"},
                       f"$.$defs.highestIssued-1.0.properties.{name}")

    extension = _validate_object_shape(
        definitions, "extensionRecord-1.0",
        ["id", "ownerId", "typeId", "schemaVersion", "subject", "mediaType",
         "referencePolicy", "payload"],
        {"id", "ownerId", "typeId", "schemaVersion", "subject", "mediaType",
         "referencePolicy", "payload"})
    _require_exact(extension["subject"], {
        "oneOf": [{"type": "null"}, {"$ref": "#/$defs/extensionTarget-1.0"}],
    }, "$.$defs.extensionRecord-1.0.properties.subject")
    target = _validate_object_shape(definitions, "extensionTarget-1.0", ["kind", "id"],
                                    {"kind", "id"})
    target_kinds = ["project", "composition", "node", "edge", "layer", "layer-slot",
                    "parameter", "animation-curve", "keyframe"]
    _require_exact(target["kind"], {"enum": target_kinds},
                   "$.$defs.extensionTarget-1.0.properties.kind")
    _validate_discriminated_union(definitions, "extensionReferencePolicy-1.0", [
        ("none", ["kind"], {"kind"}),
        ("host-table", ["kind", "references"], {"kind", "references"}),
        ("owner-remapper", ["kind", "remapperId", "version"],
         {"kind", "remapperId", "version"}),
    ])
    _validate_object_shape(definitions, "extensionHostReference-1.0", ["key", "target"],
                           {"key", "target"})
    canonical_base64 = _require_object(definitions["canonicalBase64"],
                                       "$.$defs.canonicalBase64")
    _require_exact(canonical_base64, {
        "$comment": (
            "Project I/O rejects nonzero unused tail bits and checks the decoded per-record and "
            "aggregate payload ceilings before allocation."
        ),
        "type": "string", "maxLength": 89_478_488, "pattern": _BASE64_PATTERN,
    }, "$.$defs.canonicalBase64")

    reference_properties = {
        "project-1.0": {
            "id": "objectId", "name": "humanFacingName", "colorSettings": "colorSettings-1.0",
        },
        "composition-1.0": {
            "id": "objectId", "name": "humanFacingName", "duration": "positiveRationalTime",
            "format": "compositionFormat-1.0", "graph": "graph-1.0",
        },
        "compositionFormat-1.0": {"pixelAspect": "unsignedRatio", "frameRate": "unsignedRatio"},
        "colorSettings-1.0": {
            "schemaVersion": "fixedVersion-1.0", "ocioConfig": "ocioConfigReference-1.0",
        },
        "ocioConfigReference-1.0": {
            "schemaVersion": "fixedVersion-1.0", "locator": "ocioConfigLocator-1.0",
            "expectedRevision": "ocioConfigRevision-1.0",
        },
        "parameter-1.0": {
            "id": "objectId", "schemaKey": "structuralText", "source": "parameterSource-1.0",
        },
        "scalarKeyframe-1.0": {"id": "objectId", "time": "rationalTime"},
        "vec2Keyframe-1.0": {
            "id": "objectId", "time": "rationalTime", "value": "vec2Value-1.0",
        },
        "graph-1.0": {
            "layerStack": "layerStack-1.0", "compositionOutput": "outputPortReference-1.0",
        },
        "node-1.0": {
            "id": "objectId", "typeId": "namespacedIdentifier",
            "schemaVersion": "positiveUnsigned32",
        },
        "parameterBinding-1.0": {"role": "structuralText", "parameterId": "objectId"},
        "edge-1.0": {
            "id": "objectId", "source": "outputPortReference-1.0",
            "destination": "inputPortReference-1.0",
        },
        "outputPortReference-1.0": {"nodeId": "objectId", "port": "structuralText"},
        "layerOutput-1.0": {
            "nodeId": "objectId", "layerId": "objectId", "name": "humanFacingName",
            "outputPort": "structuralText",
        },
        "layerStack-1.0": {"nodeId": "objectId"},
        "layerStackEntry-1.0": {"slotId": "objectId", "layerId": "objectId"},
        "idAllocation-1.0": {"highestIssued": "highestIssued-1.0"},
        "extensionRecord-1.0": {
            "id": "objectId", "ownerId": "namespacedIdentifier",
            "typeId": "namespacedIdentifier", "schemaVersion": "schemaVersion-1.0",
            "mediaType": "structuralText", "referencePolicy": "extensionReferencePolicy-1.0",
            "payload": "canonicalBase64",
        },
        "extensionTarget-1.0": {"id": "objectId"},
        "extensionHostReference-1.0": {
            "key": "structuralText", "target": "extensionTarget-1.0",
        },
    }
    for definition_name, expected_properties in reference_properties.items():
        definition = _require_object(definitions[definition_name],
                                     f"$.$defs.{definition_name}")
        definition_properties = _require_object(
            definition["properties"], f"$.$defs.{definition_name}.properties")
        for property_name, target in expected_properties.items():
            _require_exact(definition_properties[property_name], {"$ref": f"#/$defs/{target}"},
                           f"$.$defs.{definition_name}.properties.{property_name}")

    for keyframe_name in ("scalarKeyframe-1.0", "vec2Keyframe-1.0"):
        keyframe_properties = definitions[keyframe_name]["properties"]
        _require_exact(keyframe_properties["outgoingInterpolation"],
                       {"enum": ["hold", "linear"]},
                       f"$.$defs.{keyframe_name}.properties.outgoingInterpolation")
    _require_exact(definitions["scalarKeyframe-1.0"]["properties"]["value"],
                   {"type": "number"}, "$.$defs.scalarKeyframe-1.0.properties.value")
    _require_exact(definitions["vec2Value-1.0"]["properties"],
                   {"x": {"type": "number"}, "y": {"type": "number"}},
                   "$.$defs.vec2Value-1.0.properties")
    _require_exact(parameter_values["vec2"]["x"], {"type": "number"},
                   "$.$defs.parameterValue-1.0 vec2 x")
    _require_exact(parameter_values["vec2"]["y"], {"type": "number"},
                   "$.$defs.parameterValue-1.0 vec2 y")
    for channel in ("red", "green", "blue", "alpha"):
        _require_exact(parameter_values["color4"][channel], {"type": "number"},
                       f"$.$defs.parameterValue-1.0 color4 {channel}")
    _require_exact(parameter_values["string"]["value"], {"type": "string"},
                   "$.$defs.parameterValue-1.0 string value")
    _require_exact(parameter_values["rational"]["numerator"],
                   {"$ref": "#/$defs/signed64Decimal"},
                   "$.$defs.parameterValue-1.0 rational numerator")
    _require_exact(parameter_values["rational"]["denominator"],
                   {"$ref": "#/$defs/positiveSigned64Decimal"},
                   "$.$defs.parameterValue-1.0 rational denominator")
    for curve_kind in ("scalar", "vec2"):
        _require_exact(curves[curve_kind]["id"], {"$ref": "#/$defs/objectId"},
                       f"$.$defs.animationCurve-1.0 {curve_kind} id")

    input_alternatives = definitions["inputPortReference-1.0"]["oneOf"]
    input_expectations = [
        {"nodeId": "objectId", "port": "structuralText"},
        {"stackNodeId": "objectId", "slotId": "objectId", "role": "structuralText"},
    ]
    for index, expected_properties in enumerate(input_expectations):
        branch_properties = input_alternatives[index]["properties"]
        for property_name, target in expected_properties.items():
            _require_exact(branch_properties[property_name], {"$ref": f"#/$defs/{target}"},
                           f"$.$defs.inputPortReference-1.0.oneOf[{index}].{property_name}")

    reference_policies = definitions["extensionReferencePolicy-1.0"]["oneOf"]
    _validate_array(reference_policies[1]["properties"]["references"],
                    "$.$defs.extensionReferencePolicy-1.0 host-table references",
                    "#/$defs/extensionHostReference-1.0")
    _require_exact(reference_policies[2]["properties"]["remapperId"],
                   {"$ref": "#/$defs/namespacedIdentifier"},
                   "$.$defs.extensionReferencePolicy-1.0 owner-remapper remapperId")
    _require_exact(reference_policies[2]["properties"]["version"],
                   {"$ref": "#/$defs/schemaVersion-1.0"},
                   "$.$defs.extensionReferencePolicy-1.0 owner-remapper version")

    _validate_references(schema, schema)


def check_repository(root: Path) -> None:
    repository = root.resolve()
    validate_manifest_schema(load_strict_json(repository / _MANIFEST_SCHEMA_PATH))
    validate_document_schema(load_strict_json(repository / _DOCUMENT_SCHEMA_PATH))


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
