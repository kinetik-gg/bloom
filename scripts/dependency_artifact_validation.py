"""Validate only Bloom's checked-in synthetic dependency contract fixtures.

This is not a production lock/prefix validator: it has no reviewed Unicode 15.1 bootstrap,
complete prefix filesystem/no-follow/link inspection, or trusted identity capability type.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
from pathlib import Path
import re
from typing import Any, Sequence

from dependency_artifact_canonical import (
    ARTIFACT_LIMITS,
    DependencyArtifactError,
    encode_canonical,
    fail as _fail,
    load_schema_artifact,
    parse_canonical_fixture,
    read_bounded as _read_bounded,
    reject_fixture_as_production_path,
)
from dependency_artifact_schemas import SCHEMA_BYTE_DIGESTS, validate_schema_artifact

__all__ = [
    "DependencyArtifactError", "check_repository", "encode_canonical",
    "load_schema_artifact", "parse_canonical_fixture",
    "reject_fixture_as_production_path", "validate_lock_fixture",
    "validate_prefix_fixture", "validate_schema_artifact",
]


_SCHEMA_DIR = Path("dependencies/schemas")
_FIXTURE_DIR = Path("dependencies/tests/fixtures")
_LOCK_SCHEMA = _SCHEMA_DIR / "dependency-lock-1.0.schema.json"
_PREFIX_SCHEMA = _SCHEMA_DIR / "prefix-manifest-1.0.schema.json"
_LOCK_FIXTURE = _FIXTURE_DIR / "valid-lock.json"
_PREFIX_FIXTURE = _FIXTURE_DIR / "valid-prefix-manifest.json"
_UNICODE_FILES = [
    "UnicodeData.txt", "CompositionExclusions.txt", "DerivedNormalizationProps.txt",
    "CaseFolding.txt", "NormalizationTest.txt",
]
_IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9._-]{0,127}\Z")
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
_DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}\Z")
_DOTTED = re.compile(r"[0-9]+(?:\.[0-9]+)*\Z")
_ENVIRONMENT_NAME = re.compile(r"[A-Z_][A-Z0-9_]{0,127}\Z")
_CMAKE_OPTION = re.compile(r"[A-Za-z_][A-Za-z0-9_.-]{0,255}\Z")
_INVALID_PATH = set('\\<>:"|?*')


def _object(value: Any, keys: Sequence[str], location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail("type", location, "expected object")
    if list(value) != list(keys):
        _fail("members", location, f"expected {list(keys)!r}, got {list(value)!r}")
    return value


def _strict_equal(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return list(left) == list(right) and all(
            _strict_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _strict_equal(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return left == right


def _array(value: Any, location: str, maximum: int, minimum: int = 0) -> list[Any]:
    if not isinstance(value, list):
        _fail("type", location, "expected array")
    if not minimum <= len(value) <= maximum:
        _fail("count", location, f"expected {minimum}..{maximum} items, got {len(value)}")
    return value


def _string(value: Any, location: str, *, maximum: int = 4096,
            pattern: re.Pattern[str] | None = None, nullable: bool = False) -> str | None:
    if nullable and value is None:
        return None
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum:
        _fail("string", location, f"expected non-empty string of at most {maximum} UTF-8 bytes")
    if pattern is not None and pattern.fullmatch(value) is None:
        _fail("lexical", location, repr(value))
    return value


def _enum(value: Any, allowed: Sequence[Any], location: str) -> Any:
    if not any(type(value) is type(item) and value == item for item in allowed):
        _fail("enum", location, f"expected one of {list(allowed)!r}, got {value!r}")
    return value


def _uint(value: Any, location: str, maximum: int = 18_446_744_073_709_551_615) -> int:
    if type(value) is not int or value < 0 or value > maximum:
        _fail("uint", location, f"expected integer in 0..{maximum}")
    return value


def _ordered(values: list[Any], key, location: str) -> None:
    identities = [key(value) for value in values]
    if any(identities[index] == identities[index - 1] for index in range(1, len(identities))):
        _fail("duplicate-identity", location, repr(identities))
    if identities != sorted(identities):
        _fail("order", location, repr(identities))


def _digest(value: Any, location: str, context: dict[str, Any] | None = None) -> str:
    result = _string(value, location, maximum=71, pattern=_DIGEST)
    assert result is not None
    if context is not None and result not in context["payload_digests"]:
        _fail("fixture-digest", location, "digest does not bind a checked-in synthetic payload")
    return result


def _portable_path(value: Any, location: str) -> str:
    result = _string(value, location, maximum=1024)
    assert result is not None
    segments = result.split("/")
    reserved = {"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"}
    reserved.update(f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10))
    for segment in segments:
        if not segment or segment in (".", "..") or segment[-1] in " .":
            _fail("portable-path", location, repr(result))
        if len(segment.encode("utf-8")) > 255 or any(ord(c) < 32 or ord(c) == 127 or c in _INVALID_PATH for c in segment):
            _fail("portable-path", location, repr(result))
        if segment.split(".", 1)[0].lower() in reserved:
            _fail("portable-path", location, repr(result))
    return result


def _validate_printable_token(value: str, location: str) -> None:
    if (
        not all(0x21 <= ord(character) <= 0x7E for character in value)
        or any(character in ';"\\' for character in value)
    ):
        _fail("printable-token", location, repr(value))


def _fixture_file(path: str, digest: str, location: str,
                  context: dict[str, Any]) -> None:
    fixture_prefix = "dependencies/tests/fixtures/payloads/"
    if not path.startswith(fixture_prefix):
        _fail("fixture-separation", location, "synthetic evidence must remain below fixture payloads")
    lexical = context["root"] / path
    candidate = context["root"]
    for component in Path(path).parts:
        candidate /= component
        if candidate.is_symlink():
            _fail("fixture-evidence", location, "payload path must not traverse a symlink")
    absolute = lexical.resolve()
    try:
        absolute.relative_to(context["payload_root"])
    except ValueError:
        _fail("fixture-separation", location, "path escapes fixture payloads")
    if not absolute.is_file():
        _fail("fixture-evidence", location, "must name a checked-in regular payload")
    actual = "sha256:" + hashlib.sha256(_read_bounded(absolute, 1_048_576)).hexdigest()
    if actual != digest:
        _fail("fixture-evidence", location, f"digest must be {actual}")


def _artifact(value: Any, location: str, context: dict[str, Any]) -> tuple[str, str]:
    artifact = _object(value, ["path", "sha256"], location)
    path = _portable_path(artifact["path"], f"{location}.path")
    digest = _digest(artifact["sha256"], f"{location}.sha256")
    _fixture_file(path, digest, location, context)
    return path, digest


def _identifier(value: Any, location: str) -> str:
    result = _string(value, location, maximum=128, pattern=_IDENTIFIER)
    assert result is not None
    return result


def _date(value: Any, location: str) -> None:
    text = _string(value, location, maximum=10, pattern=_DATE)
    try:
        datetime.strptime(text, "%Y-%m-%d")
    except ValueError:
        _fail("date", location, repr(text))


def _timestamp_from_epoch(epoch: int) -> str:
    value = datetime(1970, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=epoch)
    return (
        f"{value.year:04d}-{value.month:02d}-{value.day:02d}"
        f"T{value.hour:02d}:{value.minute:02d}:{value.second:02d}Z"
    )


def _artifact_array(value: Any, location: str, context: dict[str, Any], minimum: int = 0) -> list[Any]:
    items = _array(value, location, 8192, minimum)
    for index, item in enumerate(items):
        _artifact(item, f"{location}[{index}]", context)
    _ordered(items, lambda item: item["path"].encode("utf-8"), location)
    return items


def _prefix_artifact_array(value: Any, location: str, context: dict[str, Any], minimum: int = 0) -> list[Any]:
    items = _array(value, location, 8192, minimum)
    for index, item in enumerate(items):
        reference = _object(item, ["path", "sha256"], f"{location}[{index}]")
        _portable_path(reference["path"], f"{location}[{index}].path")
        _digest(reference["sha256"], f"{location}[{index}].sha256", context)
    _ordered(items, lambda item: item["path"].encode("utf-8"), location)
    return items


def _validate_profile(value: Any, location: str) -> None:
    profile = _object(value, ["id", "target", "buildConfiguration", "consumerAbi", "toolchain", "environment", "qualificationGates"], location)
    _identifier(profile["id"], f"{location}.id")
    target = _object(profile["target"], ["triple", "operatingSystem", "architecture", "minimumOsVersion"], f"{location}.target")
    _string(target["triple"], f"{location}.target.triple")
    system = _enum(target["operatingSystem"], ["linux", "macos", "windows"], f"{location}.target.operatingSystem")
    _enum(target["architecture"], ["x86_64", "aarch64"], f"{location}.target.architecture")
    minimum_os = _string(target["minimumOsVersion"], f"{location}.target.minimumOsVersion", pattern=_DOTTED, nullable=True)
    configuration = _enum(profile["buildConfiguration"], ["debug", "release"], f"{location}.buildConfiguration")
    abi_keys = ["cxxStandard", "compilerFamily", "compilerAbi", "standardLibrary", "standardLibraryAbi", "cxxRuntime", "cxxRuntimeAbi", "cxxRuntimeLinkage", "platformRuntime", "platformRuntimeAbi", "exceptions", "rtti", "libstdcxxCxx11Abi", "msvcRuntime", "msvcIteratorDebugLevel", "windowsSdk", "windowsSdkVersion", "appleSdk", "appleSdkVersion", "appleDeploymentTarget", "abiFlags"]
    abi = _object(profile["consumerAbi"], abi_keys, f"{location}.consumerAbi")
    _enum(abi["cxxStandard"], [20], f"{location}.consumerAbi.cxxStandard")
    compiler = _enum(abi["compilerFamily"], ["gcc", "clang", "apple-clang", "msvc", "clang-cl"], f"{location}.consumerAbi.compilerFamily")
    for name in ("compilerAbi", "standardLibraryAbi", "cxxRuntimeAbi", "platformRuntimeAbi"):
        identity = _string(abi[name], f"{location}.consumerAbi.{name}")
        if any(not 0x20 <= ord(character) <= 0x7E for character in identity):
            _fail("ascii-identity", f"{location}.consumerAbi.{name}", repr(identity))
    library = _enum(abi["standardLibrary"], ["libstdc++", "libc++", "msvc-stl"], f"{location}.consumerAbi.standardLibrary")
    runtime = _enum(abi["cxxRuntime"], ["libgcc", "compiler-rt", "msvc"], f"{location}.consumerAbi.cxxRuntime")
    linkage = _enum(abi["cxxRuntimeLinkage"], ["dynamic", "static"], f"{location}.consumerAbi.cxxRuntimeLinkage")
    platform = _enum(abi["platformRuntime"], ["glibc", "musl", "ucrt", "macos-libsystem"], f"{location}.consumerAbi.platformRuntime")
    if type(abi["exceptions"]) is not bool or type(abi["rtti"]) is not bool:
        _fail("type", f"{location}.consumerAbi", "exceptions and rtti must be booleans")
    _enum(abi["libstdcxxCxx11Abi"], [0, 1, None], f"{location}.consumerAbi.libstdcxxCxx11Abi")
    _enum(abi["msvcRuntime"], ["dynamic-release", "dynamic-debug", "static-release", "static-debug", None], f"{location}.consumerAbi.msvcRuntime")
    _enum(abi["msvcIteratorDebugLevel"], [0, 1, 2, None], f"{location}.consumerAbi.msvcIteratorDebugLevel")
    for name in ("windowsSdk", "windowsSdkVersion", "appleSdk", "appleSdkVersion", "appleDeploymentTarget"):
        _string(abi[name], f"{location}.consumerAbi.{name}", pattern=_DOTTED if "Version" in name or "Target" in name else None, nullable=True)
    flags = _array(abi["abiFlags"], f"{location}.consumerAbi.abiFlags", 8192)
    for index, flag in enumerate(flags):
        text = _string(flag, f"{location}.consumerAbi.abiFlags[{index}]", maximum=512)
        _validate_printable_token(text, f"{location}.consumerAbi.abiFlags[{index}]")
    _ordered(flags, lambda item: item.encode("utf-8"), f"{location}.consumerAbi.abiFlags")
    toolchain = _object(profile["toolchain"], ["cmake", "generator", "buildTool", "compiler", "linker", "standardLibrary", "sdk"], f"{location}.toolchain")
    for name, tool in toolchain.items():
        record = _object(tool, ["name", "version", "identity"], f"{location}.toolchain.{name}")
        for field in record:
            _string(record[field], f"{location}.toolchain.{name}.{field}")
    environment = _object(profile["environment"], ["profileId", "sourceDateEpoch", "variables"], f"{location}.environment")
    _identifier(environment["profileId"], f"{location}.environment.profileId")
    _uint(environment["sourceDateEpoch"], f"{location}.environment.sourceDateEpoch", 253_402_300_799)
    variables = _array(environment["variables"], f"{location}.environment.variables", 8192)
    for index, item in enumerate(variables):
        variable = _object(item, ["name", "value"], f"{location}.environment.variables[{index}]")
        _string(variable["name"], f"{location}.environment.variables[{index}].name", maximum=128, pattern=_ENVIRONMENT_NAME)
        _string(variable["value"], f"{location}.environment.variables[{index}].value")
    _ordered(variables, lambda item: item["name"].encode("utf-8"), f"{location}.environment.variables")
    gates = _array(profile["qualificationGates"], f"{location}.qualificationGates", 4096, 1)
    for index, item in enumerate(gates):
        gate = _object(item, ["gateId", "disposition", "reason"], f"{location}.qualificationGates[{index}]")
        _identifier(gate["gateId"], f"{location}.qualificationGates[{index}].gateId")
        disposition = _enum(gate["disposition"], ["required", "not-applicable"], f"{location}.qualificationGates[{index}].disposition")
        reason = _string(gate["reason"], f"{location}.qualificationGates[{index}].reason", nullable=True)
        if (disposition == "not-applicable") != (reason is not None):
            _fail("gate-reason", f"{location}.qualificationGates[{index}]", "reason presence must match not-applicable")
    _ordered(gates, lambda item: item["gateId"].encode("utf-8"), f"{location}.qualificationGates")
    sdk = toolchain["sdk"]
    if library == "libstdc++" and abi["libstdcxxCxx11Abi"] is None or library != "libstdc++" and abi["libstdcxxCxx11Abi"] is not None:
        _fail("abi-platform", f"{location}.consumerAbi.libstdcxxCxx11Abi", "applicability mismatch")
    if system == "linux":
        if minimum_os is not None or compiler not in ("gcc", "clang") or library not in ("libstdc++", "libc++") or runtime not in ("libgcc", "compiler-rt") or platform not in ("glibc", "musl") or any(abi[name] is not None for name in ("msvcRuntime", "msvcIteratorDebugLevel", "windowsSdk", "windowsSdkVersion", "appleSdk", "appleSdkVersion", "appleDeploymentTarget")):
            _fail("abi-platform", location, "Linux profile fields disagree")
    elif system == "windows":
        expected_runtime = f"{linkage}-{configuration}"
        if minimum_os is None or compiler not in ("msvc", "clang-cl") or library != "msvc-stl" or runtime != "msvc" or platform != "ucrt" or abi["msvcRuntime"] != expected_runtime or abi["msvcIteratorDebugLevel"] is None or abi["windowsSdk"] != "windows-sdk" or abi["windowsSdkVersion"] != sdk["version"] or sdk["name"] != "windows-sdk" or any(abi[name] is not None for name in ("appleSdk", "appleSdkVersion", "appleDeploymentTarget")):
            _fail("abi-platform", location, "Windows profile fields disagree")
    elif minimum_os is None or compiler != "apple-clang" or library != "libc++" or runtime != "compiler-rt" or platform != "macos-libsystem" or abi["appleSdk"] != "macosx" or abi["appleSdkVersion"] != sdk["version"] or sdk["name"] != "macosx" or abi["appleDeploymentTarget"] != minimum_os or any(abi[name] is not None for name in ("msvcRuntime", "msvcIteratorDebugLevel", "windowsSdk", "windowsSdkVersion")):
        _fail("abi-platform", location, "macOS profile fields disagree")


def _validate_component(value: Any, location: str, context: dict[str, Any]) -> None:
    component = _object(value, ["name", "version", "source", "license", "patches", "dependencies", "profileBuilds", "securityReview"], location)
    _identifier(component["name"], f"{location}.name")
    _string(component["version"], f"{location}.version")
    source = _object(component["source"], ["url", "archiveSha256", "commit", "provenancePolicy", "provenanceReview", "provenance"], f"{location}.source")
    url = _string(source["url"], f"{location}.source.url")
    if not url.startswith("https://example.invalid/"):
        _fail("fixture-url", f"{location}.source.url", "must use https://example.invalid/")
    source_digest = _digest(source["archiveSha256"], f"{location}.source.archiveSha256")
    _fixture_file(
        "dependencies/tests/fixtures/payloads/source-archive.txt",
        source_digest,
        f"{location}.source.archiveSha256",
        context,
    )
    _string(source["commit"], f"{location}.source.commit", nullable=True)
    policy = _enum(source["provenancePolicy"], ["required", "not-published"], f"{location}.source.provenancePolicy")
    _artifact(source["provenanceReview"], f"{location}.source.provenanceReview", context)
    provenance = _array(source["provenance"], f"{location}.source.provenance", 8192)
    for index, item in enumerate(provenance):
        record = _object(item, ["kind", "evidence", "identity", "issuer", "policy"], f"{location}.source.provenance[{index}]")
        _enum(record["kind"], ["detached-signature", "sigstore-bundle", "signed-tag"], f"{location}.source.provenance[{index}].kind")
        _artifact(record["evidence"], f"{location}.source.provenance[{index}].evidence", context)
        _string(record["identity"], f"{location}.source.provenance[{index}].identity")
        _string(record["issuer"], f"{location}.source.provenance[{index}].issuer", nullable=True)
        _string(record["policy"], f"{location}.source.provenance[{index}].policy")
    _ordered(provenance, lambda item: (item["kind"].encode(), item["evidence"]["path"].encode()), f"{location}.source.provenance")
    if (policy == "required") != bool(provenance):
        _fail("provenance-policy", f"{location}.source", "policy and evidence presence disagree")
    license_value = _object(component["license"], ["spdxExpression", "licenseFiles", "copyrightFiles", "noticeFiles", "sourceObligation", "modified", "reviewRecord", "reviewedAt"], f"{location}.license")
    _string(license_value["spdxExpression"], f"{location}.license.spdxExpression")
    _artifact_array(license_value["licenseFiles"], f"{location}.license.licenseFiles", context, 1)
    _artifact_array(license_value["copyrightFiles"], f"{location}.license.copyrightFiles", context)
    _artifact_array(license_value["noticeFiles"], f"{location}.license.noticeFiles", context)
    _enum(license_value["sourceObligation"], ["none", "ship-corresponding-source", "ship-source-offer"], f"{location}.license.sourceObligation")
    if type(license_value["modified"]) is not bool:
        _fail("type", f"{location}.license.modified", "expected boolean")
    _artifact(license_value["reviewRecord"], f"{location}.license.reviewRecord", context)
    _date(license_value["reviewedAt"], f"{location}.license.reviewedAt")
    patches = _array(component["patches"], f"{location}.patches", 8192)
    for index, item in enumerate(patches):
        patch = _object(item, ["path", "sha256", "applyOrder", "reason"], f"{location}.patches[{index}]")
        patch_path = _portable_path(patch["path"], f"{location}.patches[{index}].path")
        patch_digest = _digest(patch["sha256"], f"{location}.patches[{index}].sha256")
        _fixture_file(patch_path, patch_digest, f"{location}.patches[{index}]", context)
        if _uint(patch["applyOrder"], f"{location}.patches[{index}].applyOrder", 4_294_967_295) != index:
            _fail("patch-order", f"{location}.patches[{index}].applyOrder", f"expected {index}")
        _string(patch["reason"], f"{location}.patches[{index}].reason")
    if bool(patches) != license_value["modified"]:
        _fail("modified", location, "patch presence and modified flag disagree")
    dependencies = _array(component["dependencies"], f"{location}.dependencies", 8192)
    for index, item in enumerate(dependencies):
        dependency = _object(item, ["name", "relationship"], f"{location}.dependencies[{index}]")
        _identifier(dependency["name"], f"{location}.dependencies[{index}].name")
        _enum(dependency["relationship"], ["build", "link", "runtime-plugin", "vendored"], f"{location}.dependencies[{index}].relationship")
    _ordered(dependencies, lambda item: (item["name"].encode(), item["relationship"].encode()), f"{location}.dependencies")
    builds = _array(component["profileBuilds"], f"{location}.profileBuilds", 256, 1)
    for index, item in enumerate(builds):
        build = _object(item, ["profileId", "linkage", "cmakeOptions", "featureDecisions", "capabilities", "shippingRoles", "conformanceFixtureSets"], f"{location}.profileBuilds[{index}]")
        _identifier(build["profileId"], f"{location}.profileBuilds[{index}].profileId")
        _enum(build["linkage"], ["dynamic", "static", "header-only", "executable", "data-only"], f"{location}.profileBuilds[{index}].linkage")
        for field, identity, maximum in (("cmakeOptions", "name", 8192), ("featureDecisions", "id", 8192), ("conformanceFixtureSets", "id", 8192)):
            records = _array(build[field], f"{location}.profileBuilds[{index}].{field}", maximum)
            for subindex, record_value in enumerate(records):
                if field == "cmakeOptions":
                    record = _object(record_value, ["name", "value"], f"{location}.{field}[{subindex}]")
                    _string(record["name"], f"{location}.{field}[{subindex}].name", maximum=256, pattern=_CMAKE_OPTION)
                    _string(record["value"], f"{location}.{field}[{subindex}].value")
                elif field == "featureDecisions":
                    record = _object(record_value, ["id", "state", "reason"], f"{location}.{field}[{subindex}]")
                    _identifier(record["id"], f"{location}.{field}[{subindex}].id")
                    _enum(record["state"], ["enabled", "disabled"], f"{location}.{field}[{subindex}].state")
                    _string(record["reason"], f"{location}.{field}[{subindex}].reason")
                else:
                    record = _object(record_value, ["id", "artifact"], f"{location}.{field}[{subindex}]")
                    _identifier(record["id"], f"{location}.{field}[{subindex}].id")
                    _artifact(record["artifact"], f"{location}.{field}[{subindex}].artifact", context)
            _ordered(records, lambda entry, name=identity: entry[name].encode(), f"{location}.profileBuilds[{index}].{field}")
        capabilities = _array(build["capabilities"], f"{location}.profileBuilds[{index}].capabilities", 65536)
        for subindex, capability in enumerate(capabilities):
            _identifier(capability, f"{location}.profileBuilds[{index}].capabilities[{subindex}]")
        _ordered(capabilities, lambda item: item.encode(), f"{location}.profileBuilds[{index}].capabilities")
        roles = _array(build["shippingRoles"], f"{location}.profileBuilds[{index}].shippingRoles", 8, 1)
        for subindex, role in enumerate(roles):
            _enum(role, ["library", "executable", "plugin", "data", "cmake-package", "license", "notice", "source"], f"{location}.profileBuilds[{index}].shippingRoles[{subindex}]")
        _ordered(roles, lambda item: item.encode(), f"{location}.profileBuilds[{index}].shippingRoles")
    _ordered(builds, lambda item: item["profileId"].encode(), f"{location}.profileBuilds")
    security = _object(component["securityReview"], ["reviewedAt", "record", "vulnerabilities"], f"{location}.securityReview")
    _date(security["reviewedAt"], f"{location}.securityReview.reviewedAt")
    _artifact(security["record"], f"{location}.securityReview.record", context)
    vulnerabilities = _array(security["vulnerabilities"], f"{location}.securityReview.vulnerabilities", 8192)
    for index, item in enumerate(vulnerabilities):
        record = _object(item, ["id", "disposition", "record"], f"{location}.securityReview.vulnerabilities[{index}]")
        _identifier(record["id"], f"{location}.securityReview.vulnerabilities[{index}].id")
        _enum(record["disposition"], ["not-affected", "mitigated", "accepted-risk"], f"{location}.securityReview.vulnerabilities[{index}].disposition")
        _artifact(record["record"], f"{location}.securityReview.vulnerabilities[{index}].record", context)
    _ordered(vulnerabilities, lambda item: item["id"].encode(), f"{location}.securityReview.vulnerabilities")


def validate_lock_fixture(value: Any, context: dict[str, Any]) -> None:
    lock = _object(value, ["format", "schemaVersion", "unicodeProfile", "profiles", "components"], "$")
    if lock["format"] != "org.kinetik.bloom.dependencies.lock":
        _fail("format", "$.format", repr(lock["format"]))
    version = _object(lock["schemaVersion"], ["major", "minor"], "$.schemaVersion")
    if type(version["major"]) is not int or type(version["minor"]) is not int or version != {"major": 1, "minor": 0}:
        _fail("version", "$.schemaVersion", "expected exact 1.0")
    unicode_profile = _object(lock["unicodeProfile"], ["version", "sourceUrl", "archiveSha256", "files"], "$.unicodeProfile")
    if unicode_profile["version"] != "15.1.0":
        _fail("unicode-version", "$.unicodeProfile.version", repr(unicode_profile["version"]))
    url = _string(unicode_profile["sourceUrl"], "$.unicodeProfile.sourceUrl")
    if not url.startswith("https://example.invalid/"):
        _fail("fixture-url", "$.unicodeProfile.sourceUrl", "must use https://example.invalid/")
    archive_digest = _digest(unicode_profile["archiveSha256"], "$.unicodeProfile.archiveSha256")
    _fixture_file(
        "dependencies/tests/fixtures/payloads/unicode-archive.txt",
        archive_digest,
        "$.unicodeProfile.archiveSha256",
        context,
    )
    files = _array(unicode_profile["files"], "$.unicodeProfile.files", 5, 5)
    for index, item in enumerate(files):
        record = _object(item, ["path", "sha256"], f"$.unicodeProfile.files[{index}]")
        if record["path"] != _UNICODE_FILES[index]:
            _fail("unicode-files", f"$.unicodeProfile.files[{index}].path", f"expected {_UNICODE_FILES[index]!r}")
        digest = _digest(record["sha256"], f"$.unicodeProfile.files[{index}].sha256")
        _fixture_file(
            f"dependencies/tests/fixtures/payloads/unicode/{_UNICODE_FILES[index]}",
            digest,
            f"$.unicodeProfile.files[{index}]",
            context,
        )
    profiles = _array(lock["profiles"], "$.profiles", 256, 1)
    for index, profile in enumerate(profiles):
        _validate_profile(profile, f"$.profiles[{index}]")
    _ordered(profiles, lambda item: item["id"].encode(), "$.profiles")
    components = _array(lock["components"], "$.components", 4096, 1)
    for index, component in enumerate(components):
        _validate_component(component, f"$.components[{index}]", context)
    _ordered(components, lambda item: item["name"].encode(), "$.components")
    profile_ids = {item["id"] for item in profiles}
    component_names = {item["name"] for item in components}
    for component in components:
        for build in component["profileBuilds"]:
            if build["profileId"] not in profile_ids:
                _fail("profile-reference", f"component {component['name']}", build["profileId"])
        for dependency in component["dependencies"]:
            if dependency["name"] not in component_names or dependency["name"] == component["name"]:
                _fail("dependency-reference", f"component {component['name']}", dependency["name"])
    for profile in profiles:
        profile_id = profile["id"]
        participating = {item["name"]: item for item in components if any(build["profileId"] == profile_id for build in item["profileBuilds"])}
        if not participating:
            _fail("profile-empty", f"profile {profile_id}", "no participating component")
        owners: dict[str, str] = {}
        for name, component in participating.items():
            if any(edge["name"] not in participating for edge in component["dependencies"]):
                _fail("dependency-closure", f"profile {profile_id}", name)
            build = next(item for item in component["profileBuilds"] if item["profileId"] == profile_id)
            for capability in build["capabilities"]:
                if capability in owners:
                    _fail("capability-owner", f"profile {profile_id}", capability)
                owners[capability] = name
        visiting: set[str] = set()
        visited: set[str] = set()
        def visit(name: str) -> None:
            if name in visiting:
                _fail("dependency-cycle", f"profile {profile_id}", name)
            if name in visited:
                return
            visiting.add(name)
            for edge in participating[name]["dependencies"]:
                visit(edge["name"])
            visiting.remove(name)
            visited.add(name)
        for name in sorted(participating):
            visit(name)


def _identity(domain: bytes, encoded: bytes) -> str:
    """Compute an untrusted synthetic test vector; validation grants no capability."""
    return "sha256:" + hashlib.sha256(domain + b"\0" + len(encoded).to_bytes(8, "big") + encoded).hexdigest()


def _lock_identity_vector(encoded: bytes) -> str:
    return _identity(b"bloom.dependencies.lock.v1", encoded)


def _build_options_identity_vector(profile_build: dict[str, Any]) -> str:
    return _identity(b"bloom.dependencies.component-build.v1", encode_canonical(profile_build))


def _prefix_identity_vector(encoded: bytes) -> str:
    return _identity(b"bloom.dependencies.prefix.v1", encoded)


def _component_artifacts(component: dict[str, Any]) -> list[tuple[str, str, str]]:
    result: list[tuple[str, str, str]] = []
    def add(reference: dict[str, str], role: str) -> None:
        result.append((reference["path"], reference["sha256"], role))
    source = component["source"]
    add(source["provenanceReview"], "qualification-evidence")
    for record in source["provenance"]:
        add(record["evidence"], "qualification-evidence")
    license_value = component["license"]
    for field in ("licenseFiles", "copyrightFiles"):
        for reference in license_value[field]:
            add(reference, "license")
    for reference in license_value["noticeFiles"]:
        add(reference, "notice")
    add(license_value["reviewRecord"], "qualification-evidence")
    for patch in component["patches"]:
        result.append((patch["path"], patch["sha256"], "qualification-evidence"))
    for build in component["profileBuilds"]:
        for fixture_set in build["conformanceFixtureSets"]:
            add(fixture_set["artifact"], "qualification-evidence")
    add(component["securityReview"]["record"], "qualification-evidence")
    for vulnerability in component["securityReview"]["vulnerabilities"]:
        add(vulnerability["record"], "qualification-evidence")
    return result


def validate_prefix_fixture(value: Any, lock: dict[str, Any], lock_encoded: bytes,
                            context: dict[str, Any]) -> None:
    prefix = _object(value, ["format", "schemaVersion", "lockIdentity", "profile", "unicodeProfile", "components", "capabilities", "cmakePackages", "installedFiles", "qualificationResults"], "$")
    if prefix["format"] != "org.kinetik.bloom.dependencies.prefix":
        _fail("format", "$.format", repr(prefix["format"]))
    version = _object(prefix["schemaVersion"], ["major", "minor"], "$.schemaVersion")
    if not _strict_equal(version, {"major": 1, "minor": 0}):
        _fail("version", "$.schemaVersion", "expected exact 1.0")
    expected_lock_identity = _lock_identity_vector(lock_encoded)
    if prefix["lockIdentity"] != expected_lock_identity:
        _fail("lock-identity", "$.lockIdentity", f"expected {expected_lock_identity}")
    matching_profiles = [item for item in lock["profiles"] if _strict_equal(prefix["profile"], item)]
    if len(matching_profiles) != 1 or not _strict_equal(prefix["unicodeProfile"], lock["unicodeProfile"]):
        _fail("lock-copy", "$", "profile or Unicode profile differs from lock")
    profile = matching_profiles[0]
    profile_id = profile["id"]
    locked_components = {item["name"]: item for item in lock["components"] if any(build["profileId"] == profile_id for build in item["profileBuilds"])}
    components = _array(prefix["components"], "$.components", 4096, 1)
    for index, result in enumerate(components):
        record = _object(result, ["name", "version", "sourceArchiveSha256", "sourceCommit", "patches", "buildOptionsIdentity"], f"$.components[{index}]")
        _identifier(record["name"], f"$.components[{index}].name")
        if record["name"] not in locked_components:
            _fail("component-coverage", f"$.components[{index}].name", repr(record["name"]))
        locked = locked_components[record["name"]]
        build = next(item for item in locked["profileBuilds"] if item["profileId"] == profile_id)
        expected = {
            "name": locked["name"],
            "version": locked["version"],
            "sourceArchiveSha256": locked["source"]["archiveSha256"],
            "sourceCommit": locked["source"]["commit"],
            "patches": locked["patches"],
            "buildOptionsIdentity": _build_options_identity_vector(build),
        }
        if not _strict_equal(record, expected):
            _fail("component-copy", f"$.components[{index}]", "result differs from lock")
    _ordered(components, lambda item: item["name"].encode(), "$.components")
    if [item["name"] for item in components] != sorted(locked_components):
        _fail("component-coverage", "$.components", "must exactly cover participating components")
    expected_capabilities: list[tuple[str, str]] = []
    for name, component in locked_components.items():
        build = next(item for item in component["profileBuilds"] if item["profileId"] == profile_id)
        expected_capabilities.extend((capability, name) for capability in build["capabilities"])
    expected_capabilities.sort()
    capabilities = _array(prefix["capabilities"], "$.capabilities", 65536)
    for index, item in enumerate(capabilities):
        record = _object(item, ["id", "providerComponent"], f"$.capabilities[{index}]")
        _identifier(record["id"], f"$.capabilities[{index}].id")
        _identifier(record["providerComponent"], f"$.capabilities[{index}].providerComponent")
    _ordered(capabilities, lambda item: item["id"].encode(), "$.capabilities")
    if [(item["id"], item["providerComponent"]) for item in capabilities] != expected_capabilities:
        _fail("capability-copy", "$.capabilities", "does not equal locked capability union")
    installed = _array(prefix["installedFiles"], "$.installedFiles", 200000)
    installed_by_path: dict[str, dict[str, Any]] = {}
    total = 0
    participating = set(locked_components)
    for index, item in enumerate(installed):
        record = _object(item, ["path", "type", "component", "role", "size", "sha256", "permissions", "linkTarget"], f"$.installedFiles[{index}]")
        path = _portable_path(record["path"], f"$.installedFiles[{index}].path")
        entry_type = _enum(record["type"], ["regular-file", "directory", "symbolic-link"], f"$.installedFiles[{index}].type")
        if record["component"] is not None:
            component = _identifier(record["component"], f"$.installedFiles[{index}].component")
            if component not in participating:
                _fail("installed-component", f"$.installedFiles[{index}].component", repr(component))
        role = _enum(record["role"], ["directory", "library", "executable", "plugin", "data", "cmake-package", "license", "notice", "source", "qualification-evidence"], f"$.installedFiles[{index}].role")
        if record["component"] is None and role not in ("directory", "qualification-evidence"):
            _fail("installed-component", f"$.installedFiles[{index}]", "null component cannot own a shipping role")
        if entry_type == "regular-file":
            size = _uint(record["size"], f"$.installedFiles[{index}].size", 4_294_967_296)
            digest = _digest(record["sha256"], f"$.installedFiles[{index}].sha256", context)
            if context["payload_sizes"][digest] != size:
                _fail("fixture-size", f"$.installedFiles[{index}].size", f"expected {context['payload_sizes'][digest]}")
            total += size
            if total > 17_179_869_184:
                _fail("installed-bytes", "$.installedFiles", "aggregate exceeds 16 GiB")
            if record["permissions"] not in ("regular", "executable") or record["linkTarget"] is not None or role == "directory":
                _fail("installed-tuple", f"$.installedFiles[{index}]", "invalid regular-file tuple")
        elif entry_type == "directory":
            if [record["size"], record["sha256"], record["permissions"], record["linkTarget"], role] != [None, None, "none", None, "directory"]:
                _fail("installed-tuple", f"$.installedFiles[{index}]", "invalid directory tuple")
        else:
            _fail(
                "fixture-symlink",
                f"$.installedFiles[{index}]",
                "synthetic fixtures cannot establish production filesystem link evidence",
            )
        installed_by_path[path] = record
    _ordered(installed, lambda item: item["path"].encode(), "$.installedFiles")
    collision_records: dict[str, dict[str, Any]] = {}
    for record in installed:
        collision_key = record["path"].lower()
        if collision_key in collision_records:
            _fail("portable-collision", record["path"], "installed path collision")
        collision_records[collision_key] = record
    for collision_key, record in collision_records.items():
        segments = collision_key.split("/")
        for end in range(1, len(segments)):
            parent = collision_records.get("/".join(segments[:end]))
            if parent is not None and parent["type"] != "directory":
                _fail("path-prefix", record["path"], "non-directory path is an ancestor")
    expected_copies: list[tuple[str, str, str, str | None]] = []
    for name, component in locked_components.items():
        expected_copies.extend(("share/bloom/dependencies/evidence/" + path.removeprefix("dependencies/"), digest, role, name) for path, digest, role in _component_artifacts(component))
    expected_copies.extend((f"share/bloom/dependencies/unicode/15.1.0/{item['path']}", item["sha256"], "qualification-evidence", None) for item in lock["unicodeProfile"]["files"])
    for path, digest, role, component in expected_copies:
        record = installed_by_path.get(path)
        if record is None or [record["type"], record["sha256"], record["role"], record["component"]] != ["regular-file", digest, role, component]:
            _fail("evidence-copy", path, "missing or mismatched installed record")
    packages = _array(prefix["cmakePackages"], "$.cmakePackages", 4096)
    for index, item in enumerate(packages):
        package = _object(item, ["name", "version", "configPath", "targets"], f"$.cmakePackages[{index}]")
        _string(package["name"], f"$.cmakePackages[{index}].name")
        _string(package["version"], f"$.cmakePackages[{index}].version")
        config_path = _portable_path(package["configPath"], f"$.cmakePackages[{index}].configPath")
        config_record = installed_by_path.get(config_path)
        if config_record is None or [config_record["type"], config_record["role"]] != ["regular-file", "cmake-package"]:
            _fail("cmake-config", f"$.cmakePackages[{index}].configPath", "must name installed cmake-package")
        targets = _array(package["targets"], f"$.cmakePackages[{index}].targets", 8192, 1)
        for target_index, target in enumerate(targets):
            text = _string(target, f"$.cmakePackages[{index}].targets[{target_index}]", maximum=256)
            _validate_printable_token(text, f"$.cmakePackages[{index}].targets[{target_index}]")
        _ordered(targets, lambda target: target.encode(), f"$.cmakePackages[{index}].targets")
    _ordered(packages, lambda item: item["name"].encode(), "$.cmakePackages")
    gates = {item["gateId"]: item for item in profile["qualificationGates"]}
    results = _array(prefix["qualificationResults"], "$.qualificationResults", 4096, 1)
    completed = _timestamp_from_epoch(profile["environment"]["sourceDateEpoch"])
    for index, item in enumerate(results):
        result = _object(item, ["gateId", "status", "evidence", "completedAt"], f"$.qualificationResults[{index}]")
        gate_id = _identifier(result["gateId"], f"$.qualificationResults[{index}].gateId")
        if gate_id not in gates:
            _fail("gate-coverage", f"$.qualificationResults[{index}].gateId", repr(gate_id))
        status = _enum(result["status"], ["passed", "failed", "not-applicable"], f"$.qualificationResults[{index}].status")
        expected_status = "passed" if gates[gate_id]["disposition"] == "required" else "not-applicable"
        if status != expected_status or result["completedAt"] != completed:
            _fail("qualification", f"$.qualificationResults[{index}]", "status or deterministic timestamp mismatch")
        evidence = _prefix_artifact_array(result["evidence"], f"$.qualificationResults[{index}].evidence", context, 1)
        for reference in evidence:
            record = installed_by_path.get(reference["path"])
            if record is None or [record["type"], record["role"], record["sha256"]] != ["regular-file", "qualification-evidence", reference["sha256"]]:
                _fail("gate-evidence", reference["path"], "does not match installed qualification evidence")
    _ordered(results, lambda item: item["gateId"].encode(), "$.qualificationResults")
    if [item["gateId"] for item in results] != sorted(gates):
        _fail("gate-coverage", "$.qualificationResults", "gate IDs differ from lock")
    for name, component in locked_components.items():
        build = next(item for item in component["profileBuilds"] if item["profileId"] == profile_id)
        actual_roles = {
            item["role"] for item in installed
            if item["component"] == name
            and item["role"] not in ("directory", "qualification-evidence")
        }
        if actual_roles != set(build["shippingRoles"]):
            _fail("shipping-role", f"component {name}", "installed shipping roles differ from lock")


def _fixture_context(root: Path) -> dict[str, Any]:
    root = root.resolve()
    payload_root = (root / _FIXTURE_DIR / "payloads").resolve()
    payloads = sorted(path for path in payload_root.rglob("*") if path.is_file() and not path.is_symlink())
    if not payloads:
        _fail("fixture-payloads", str(payload_root), "no synthetic payloads")
    sizes = {
        "sha256:" + hashlib.sha256(_read_bounded(path, 1_048_576)).hexdigest(): path.stat().st_size
        for path in payloads
    }
    return {
        "root": root,
        "payload_root": payload_root,
        "payload_digests": set(sizes),
        "payload_sizes": sizes,
    }


def check_repository(root: Path) -> tuple[str, str]:
    root = root.resolve()
    for kind, relative in (("lock", _LOCK_SCHEMA), ("prefix", _PREFIX_SCHEMA)):
        value, encoded = load_schema_artifact(root / relative)
        if hashlib.sha256(encoded).hexdigest() != SCHEMA_BYTE_DIGESTS[kind]:
            _fail("schema-bytes", str(relative), "readable schema bytes differ from frozen artifact")
        validate_schema_artifact(value, kind)
    context = _fixture_context(root)
    lock_encoded = _read_bounded(root / _LOCK_FIXTURE, ARTIFACT_LIMITS["lock"][0])
    lock = parse_canonical_fixture(lock_encoded, "lock")
    validate_lock_fixture(lock, context)
    prefix_encoded = _read_bounded(root / _PREFIX_FIXTURE, ARTIFACT_LIMITS["prefix"][0])
    prefix = parse_canonical_fixture(prefix_encoded, "prefix")
    validate_prefix_fixture(prefix, lock, lock_encoded, context)
    # These are fixture test vectors returned only after the complete synthetic pair passes.
    return _lock_identity_vector(lock_encoded), _prefix_identity_vector(prefix_encoded)
