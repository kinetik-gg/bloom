"""Exact checked-in schema artifact rules for Bloom dependency contracts."""

from __future__ import annotations

import hashlib
import json
from typing import Any

from dependency_artifact_canonical import fail


SCHEMA_BYTE_DIGESTS = {
    "lock": "7c8ed1b0c90a39d7ce4f6309907009476a6872cf9034e0376b9e6fe9cf41e99c",
    "prefix": "af0a70721fa4d01047d8b98f2a0816fea1ef742f8e3404ea5c61ee2c14bc29d2",
}
_SCHEMA_VALUE_DIGESTS = {
    "lock": "8a5ccf4803a96de9f1587469a17e2cf02ab2075dbe08c26fefa1044adf72817a",
    "prefix": "649ff2a30e0c6a39ebe1fb276325b193cb830ba7c6bc3ff991de584c9e9b50c6",
}
_DRAFT = "https://json-schema.org/draft/2020-12/schema"
_SCHEMA_IDS = {
    "lock": "urn:kinetik:bloom:schema:dependency-lock:1.0",
    "prefix": "urn:kinetik:bloom:schema:dependency-prefix-manifest:1.0",
}

__all__ = ["SCHEMA_BYTE_DIGESTS", "schema_fingerprint", "validate_schema_artifact"]


def schema_fingerprint(value: dict[str, Any]) -> str:
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def validate_schema_artifact(value: dict[str, Any], kind: str) -> None:
    expected_id = _SCHEMA_IDS[kind]
    if value.get("$schema") != _DRAFT or value.get("$id") != expected_id:
        fail("schema-identity", "$", f"expected Draft 2020-12 schema {expected_id}")
    def visit(node: Any, location: str) -> None:
        if isinstance(node, dict):
            reference = node.get("$ref")
            if reference is not None:
                if not isinstance(reference, str):
                    fail("schema-ref", f"{location}.$ref", "reference must be a string")
                allowed = reference.startswith("#/") or (
                    kind == "prefix"
                    and reference.startswith("dependency-lock-1.0.schema.json#/")
                )
                if not allowed:
                    fail("schema-ref", f"{location}.$ref", "reference is not repository-local")
            if node.get("type") == "object":
                properties = node.get("properties")
                if not isinstance(properties, dict) or node.get("required") != list(properties):
                    fail("schema-object", location, "required members must equal property order")
                if node.get("unevaluatedProperties") is not False:
                    fail("schema-object", location, "object must be closed")
            for key, child in node.items():
                visit(child, f"{location}.{key}")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                visit(child, f"{location}[{index}]")

    visit(value, "$")
    if schema_fingerprint(value) != _SCHEMA_VALUE_DIGESTS[kind]:
        fail("schema-contract", "$", f"{kind} schema value differs from the frozen artifact")
