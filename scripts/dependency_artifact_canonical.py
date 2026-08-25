"""Bounded canonical JSON primitives for Bloom dependency fixture validation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
from typing import Any, NoReturn


ARTIFACT_LIMITS = {
    "lock": (16_777_216, 64, 1_000_000),
    "prefix": (134_217_728, 64, 4_000_000),
}

__all__ = [
    "ARTIFACT_LIMITS",
    "DependencyArtifactError",
    "encode_canonical",
    "fail",
    "load_schema_artifact",
    "parse_canonical_fixture",
    "read_bounded",
    "reject_fixture_as_production_path",
]


class DependencyArtifactError(ValueError):
    """A deterministic dependency-artifact validation failure."""


def fail(code: str, location: str, detail: str) -> NoReturn:
    raise DependencyArtifactError(f"{code}: {location}: {detail}")


def read_bounded(path: Path, maximum: int) -> bytes:
    try:
        size = path.stat().st_size
    except OSError as error:
        fail("read", str(path), str(error))
    if size > maximum:
        fail("resource-bytes", str(path), f"{size} exceeds {maximum}")
    try:
        with path.open("rb") as stream:
            encoded = stream.read(maximum + 1)
    except OSError as error:
        fail("read", str(path), str(error))
    if len(encoded) > maximum:
        fail("resource-bytes", str(path), f"input exceeds {maximum}")
    return encoded


def reject_fixture_as_production_path(path: Path, expected: Path,
                                      fixture_root: Path) -> None:
    """Enforce only exact-path fixture separation, never production validity."""
    if os.pardir in path.parts:
        fail("production-path", str(path), "lexical parent-directory aliases are forbidden")
    resolved = path.resolve()
    try:
        resolved.relative_to(fixture_root.resolve())
    except ValueError:
        pass
    else:
        fail("fixture-separation", str(path), "fixture cannot be used as a production artifact")
    lexical = path if path.is_absolute() else Path.cwd() / path
    expected_lexical = expected if expected.is_absolute() else Path.cwd() / expected
    if lexical != expected_lexical or path.is_symlink():
        fail("production-path", str(path), f"expected exact path {expected}")


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate-member", "$", repr(key))
        result[key] = value
    return result


def load_schema_artifact(path: Path) -> tuple[dict[str, Any], bytes]:
    encoded = read_bounded(path, 2 * 1024 * 1024)
    if encoded.startswith(b"\xef\xbb\xbf"):
        fail("utf8-bom", str(path), "UTF-8 BOM is forbidden")
    try:
        text = encoded.decode("utf-8", errors="strict")
        value = json.loads(text, object_pairs_hook=_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail("schema-json", str(path), str(error))
    if not isinstance(value, dict):
        fail("schema-shape", "$", "schema root must be an object")
    return value, encoded


class _CanonicalParser:
    def __init__(self, encoded: bytes, kind: str, *, limits: tuple[int, int, int] | None) -> None:
        byte_limit, self.depth_limit, self.value_limit = limits or ARTIFACT_LIMITS[kind]
        if len(encoded) > byte_limit:
            fail("resource-bytes", "$", f"{len(encoded)} exceeds {byte_limit}")
        if encoded.startswith(b"\xef\xbb\xbf"):
            fail("utf8-bom", "$", "UTF-8 BOM is forbidden")
        self.data = encoded
        self.index = 0
        self.values = 0

    def parse(self) -> Any:
        value = self._value(1, "$")
        if self.index != len(self.data):
            fail("canonical-trailing", "$", f"unexpected byte at {self.index}")
        return value

    def _value(self, depth: int, location: str) -> Any:
        if depth > self.depth_limit:
            fail("resource-depth", location, f"depth exceeds {self.depth_limit}")
        self.values += 1
        if self.values > self.value_limit:
            fail("resource-values", location, f"value count exceeds {self.value_limit}")
        if self.index >= len(self.data):
            fail("json-eof", location, "expected value")
        byte = self.data[self.index]
        if byte == 0x7B:
            return self._object(depth, location)
        if byte == 0x5B:
            return self._array(depth, location)
        if byte == 0x22:
            return self._string(location)
        for token, value in ((b"true", True), (b"false", False), (b"null", None)):
            if self.data.startswith(token, self.index):
                self.index += len(token)
                return value
        if 0x30 <= byte <= 0x39:
            return self._integer(location)
        fail("canonical-token", location, f"unexpected byte 0x{byte:02x} at {self.index}")

    def _object(self, depth: int, location: str) -> dict[str, Any]:
        self.index += 1
        result: dict[str, Any] = {}
        if self._take(0x7D):
            return result
        while True:
            if self.index >= len(self.data) or self.data[self.index] != 0x22:
                fail("canonical-member", location, f"expected quoted key at {self.index}")
            key = self._string(f"{location} object key")
            if key in result:
                fail("duplicate-member", location, repr(key))
            if not self._take(0x3A):
                fail("canonical-colon", location, f"expected ':' at {self.index}")
            result[key] = self._value(depth + 1, f"{location}.{key}")
            if self._take(0x7D):
                return result
            if not self._take(0x2C):
                fail("canonical-comma", location, f"expected ',' at {self.index}")

    def _array(self, depth: int, location: str) -> list[Any]:
        self.index += 1
        result: list[Any] = []
        if self._take(0x5D):
            return result
        while True:
            if len(result) >= 200_000:
                fail("resource-array", location, "item count exceeds 200000")
            result.append(self._value(depth + 1, f"{location}[{len(result)}]"))
            if self._take(0x5D):
                return result
            if not self._take(0x2C):
                fail("canonical-comma", location, f"expected ',' at {self.index}")

    def _string(self, location: str) -> str:
        self.index += 1
        characters: list[str] = []
        decoded_bytes = 0
        short = {
            0x22: '"', 0x5C: "\\", 0x62: "\b", 0x66: "\f",
            0x6E: "\n", 0x72: "\r", 0x74: "\t",
        }
        while self.index < len(self.data):
            byte = self.data[self.index]
            if byte == 0x22:
                self.index += 1
                value = "".join(characters)
                if any(ord(character) > 0x7F for character in value):
                    fail("unicode-bootstrap", location, "synthetic fixtures must remain ASCII")
                return value
            if byte == 0x5C:
                self.index += 1
                if self.index >= len(self.data):
                    fail("canonical-escape", location, "truncated escape")
                escape = self.data[self.index]
                self.index += 1
                if escape in short:
                    characters.append(short[escape])
                    decoded_bytes += 1
                elif escape == 0x75:
                    digits = self.data[self.index:self.index + 4]
                    if len(digits) != 4 or not re.fullmatch(rb"00[0-9a-f]{2}", digits):
                        fail(
                            "canonical-escape", location,
                            "only lowercase \\u00xx controls are canonical",
                        )
                    codepoint = int(digits, 16)
                    if codepoint > 0x1F or codepoint in (8, 9, 10, 12, 13):
                        fail("canonical-escape", location, "control has a shorter canonical escape")
                    self.index += 4
                    characters.append(chr(codepoint))
                    decoded_bytes += 1
                else:
                    fail("canonical-escape", location, f"non-canonical escape at {self.index - 1}")
            elif byte < 0x20:
                fail("canonical-control", location, f"raw control at {self.index}")
            elif byte < 0x80:
                characters.append(chr(byte))
                decoded_bytes += 1
                self.index += 1
            else:
                length = 2 if byte < 0xE0 else 3 if byte < 0xF0 else 4
                raw = self.data[self.index:self.index + length]
                try:
                    characters.append(raw.decode("utf-8", errors="strict"))
                except UnicodeDecodeError as error:
                    fail("utf8", location, str(error))
                decoded_bytes += length
                self.index += length
            if decoded_bytes > 1_048_576:
                fail("resource-string", location, "decoded UTF-8 exceeds 1048576 bytes")
        fail("json-eof", location, "unterminated string")

    def _integer(self, location: str) -> int:
        start = self.index
        while self.index < len(self.data) and 0x30 <= self.data[self.index] <= 0x39:
            self.index += 1
        token = self.data[start:self.index]
        if len(token) > 1 and token[0] == 0x30:
            fail("canonical-integer", location, "leading zero")
        maximum = b"18446744073709551615"
        if len(token) > len(maximum) or len(token) == len(maximum) and token > maximum:
            fail("uint64", location, "integer exceeds uint64")
        return int(token)

    def _take(self, byte: int) -> bool:
        if self.index < len(self.data) and self.data[self.index] == byte:
            self.index += 1
            return True
        return False


def parse_canonical_fixture(encoded: bytes, kind: str, *,
                            limits: tuple[int, int, int] | None = None) -> Any:
    value = _CanonicalParser(encoded, kind, limits=limits).parse()
    if encode_canonical(value) != encoded:
        fail("canonical-bytes", "$", "typed value does not reproduce input")
    return value


def encode_canonical(value: Any) -> bytes:
    if value is None:
        return b"null"
    if value is True:
        return b"true"
    if value is False:
        return b"false"
    if type(value) is int:
        return str(value).encode("ascii")
    if isinstance(value, str):
        escapes = {
            '"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f",
            "\n": "\\n", "\r": "\\r", "\t": "\\t",
        }
        encoded = "".join(
            escapes.get(
                character,
                f"\\u{ord(character):04x}" if ord(character) < 0x20 else character,
            )
            for character in value
        )
        return b'"' + encoded.encode("utf-8") + b'"'
    if isinstance(value, list):
        return b"[" + b",".join(encode_canonical(child) for child in value) + b"]"
    if isinstance(value, dict):
        return b"{" + b",".join(
            encode_canonical(key) + b":" + encode_canonical(child)
            for key, child in value.items()
        ) + b"}"
    fail("type", "$", f"unsupported value type {type(value).__name__}")
