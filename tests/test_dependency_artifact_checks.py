#!/usr/bin/env python3
"""Adversarial self-tests for Bloom's offline dependency-artifact checker."""

from __future__ import annotations

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import types
import unittest


_ROOT = Path(__file__).resolve().parents[1]


def _load_checker() -> types.ModuleType:
    path = _ROOT / "scripts" / "dependency_artifact_validation.py"
    spec = importlib.util.spec_from_file_location("dependency_artifact_validation", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


class DependencyArtifactCheckerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.checker = _load_checker()
        cls.context = cls.checker._fixture_context(_ROOT)
        cls.lock_bytes = (
            _ROOT / "dependencies" / "tests" / "fixtures" / "valid-lock.json"
        ).read_bytes()
        cls.prefix_bytes = (
            _ROOT
            / "dependencies"
            / "tests"
            / "fixtures"
            / "valid-prefix-manifest.json"
        ).read_bytes()
        cls.lock = cls.checker.parse_canonical_fixture(cls.lock_bytes, "lock")
        cls.prefix = cls.checker.parse_canonical_fixture(cls.prefix_bytes, "prefix")

    def assertFailure(self, code: str, operation) -> str:  # type: ignore[no-untyped-def]
        with self.assertRaises(self.checker.DependencyArtifactError) as raised:
            operation()
        rendered = str(raised.exception)
        self.assertTrue(rendered.startswith(f"{code}:"), rendered)
        return rendered

    def validate_lock(self, value) -> None:  # type: ignore[no-untyped-def]
        self.checker.validate_lock_fixture(value, self.context)

    def validate_prefix(self, value) -> None:  # type: ignore[no-untyped-def]
        self.checker.validate_prefix_fixture(
            value,
            self.lock,
            self.lock_bytes,
            self.context,
        )

    def extra_installed(self, path: str, *, component=None,
                        role: str = "qualification-evidence") -> dict:  # type: ignore[no-untyped-def]
        return {
            "path": path,
            "type": "regular-file",
            "component": component,
            "role": role,
            "size": 40,
            "sha256": self.lock["components"][0]["source"]["archiveSha256"],
            "permissions": "regular",
            "linkTarget": None,
        }

    def test_checked_in_artifacts_and_identity_vectors_pass(self) -> None:
        lock_identity, prefix_identity = self.checker.check_repository(_ROOT)
        self.assertEqual(
            lock_identity,
            "sha256:3a12df1cecff55b171ee3351dcad80fbb5cb52f44924c95ce5056535709d8d79",
        )
        self.assertEqual(
            prefix_identity,
            "sha256:af0d91ce2f5004be73aec1635cf95b83733252fceef733c1b3289f1bdd8ac149",
        )
        builds = [
            self.checker._build_options_identity_vector(component["profileBuilds"][0])
            for component in self.lock["components"]
        ]
        self.assertEqual(
            builds,
            [
                "sha256:5a014d1740b6ca8189d1195c2f670d22ac6e94367f33d18808fa84094f4c424b",
                "sha256:5bc93726991d01181405854fcd7f261ca5b2807cdd09a1b5beb6973985f5b4e5",
            ],
        )
        self.assertNotEqual(
            lock_identity,
            self.checker._lock_identity_vector(self.lock_bytes + b" "),
        )

    def test_schema_loader_rejects_duplicate_decoded_members_and_bom(self) -> None:
        cases = {
            "duplicate": b'{"type":"object","properties":{},"properties":{}}',
            "bom": b"\xef\xbb\xbf{}",
        }
        expected = {"duplicate": "duplicate-member", "bom": "utf8-bom"}
        with tempfile.TemporaryDirectory() as directory:
            for name, encoded in cases.items():
                with self.subTest(name=name):
                    path = Path(directory) / f"{name}.json"
                    path.write_bytes(encoded)
                    self.assertFailure(
                        expected[name], lambda path=path: self.checker.load_schema_artifact(path)
                    )

    def test_schema_values_are_exact_closed_and_offline(self) -> None:
        for kind, name in (
            ("lock", "dependency-lock-1.0.schema.json"),
            ("prefix", "prefix-manifest-1.0.schema.json"),
        ):
            with self.subTest(kind=kind):
                schema, _ = self.checker.load_schema_artifact(
                    _ROOT / "dependencies" / "schemas" / name
                )
                self.checker.validate_schema_artifact(schema, kind)
                changed = deepcopy(schema)
                changed["unevaluatedProperties"] = True
                self.assertFailure(
                    "schema-object",
                    lambda changed=changed, kind=kind: self.checker.validate_schema_artifact(
                        changed, kind
                    ),
                )
                serialized = json.dumps(schema)
                self.assertNotIn('"$ref": "http', serialized)

        lock_schema, _ = self.checker.load_schema_artifact(
            _ROOT / "dependencies" / "schemas" / "dependency-lock-1.0.schema.json"
        )
        non_string_reference = deepcopy(lock_schema)
        non_string_reference["properties"]["schemaVersion"]["$ref"] = 7
        self.assertFailure(
            "schema-ref",
            lambda: self.checker.validate_schema_artifact(non_string_reference, "lock"),
        )

    def test_schema_printable_token_patterns_exclude_forbidden_bytes(self) -> None:
        lock_schema, _ = self.checker.load_schema_artifact(
            _ROOT / "dependencies" / "schemas" / "dependency-lock-1.0.schema.json"
        )
        prefix_schema, _ = self.checker.load_schema_artifact(
            _ROOT / "dependencies" / "schemas" / "prefix-manifest-1.0.schema.json"
        )
        for pattern in (
            lock_schema["$defs"]["abiFlag"]["pattern"],
            prefix_schema["$defs"]["cmakeTarget"]["pattern"],
        ):
            self.assertIsNotNone(re.fullmatch(pattern, "Bloom::Target"))
            for forbidden in ("has space", "semi;colon", 'quo"te', "back\\slash"):
                self.assertIsNone(re.fullmatch(pattern, forbidden))
        identity_pattern = lock_schema["$defs"]["asciiIdentity"]["pattern"]
        self.assertIsNotNone(re.fullmatch(identity_pattern, "gcc abi synthetic"))
        self.assertIsNone(re.fullmatch(identity_pattern, "nul\0identity"))
        self.assertIsNone(re.fullmatch(identity_pattern, "non-ascii-é"))

    def test_canonical_parser_rejects_noncanonical_spellings(self) -> None:
        cases = [
            (b'{"a": 1}', "canonical-token"),
            (b'{"a":1}\n', "canonical-trailing"),
            (b"\xef\xbb\xbf{}", "utf8-bom"),
            (b'{"a":0,"a":1}', "duplicate-member"),
            (b'{"a":"\\/"}', "canonical-escape"),
            (b'{"a":"\\u0061"}', "canonical-escape"),
            (b'{"a":"\\u000A"}', "canonical-escape"),
            (b'{"a":"\\u000a"}', "canonical-escape"),
            (b'{"a":01}', "canonical-integer"),
            (b'{"a":-1}', "canonical-token"),
            (b'{"a":1.0}', "canonical-comma"),
            ('{"a":"é"}'.encode(), "unicode-bootstrap"),
        ]
        for encoded, code in cases:
            with self.subTest(encoded=encoded):
                self.assertFailure(
                    code,
                    lambda encoded=encoded: self.checker.parse_canonical_fixture(
                        encoded, "lock"
                    ),
                )
        self.assertEqual(
            self.checker.parse_canonical_fixture(
                b'{"s":"\\b\\f\\n\\r\\t\\u0000"}', "lock"
            ),
            {"s": "\b\f\n\r\t\0"},
        )

    def test_checked_in_adversarial_fixture_diagnostics_are_stable(self) -> None:
        directory = (
            _ROOT / "dependencies" / "tests" / "fixtures" / "adversarial"
        )
        expected = {
            "duplicate-member.json": "duplicate-member",
            "escaped-solidus.json": "canonical-escape",
            "leading-zero.json": "canonical-integer",
            "trailing-newline.json": "canonical-trailing",
            "whitespace.json": "canonical-token",
        }
        for name, code in expected.items():
            with self.subTest(name=name):
                rendered = self.assertFailure(
                    code,
                    lambda name=name: self.checker.parse_canonical_fixture(
                        (directory / name).read_bytes(), "lock"
                    ),
                )
                self.assertEqual(rendered.split(":", 1)[0], code)

    def test_parser_checks_resource_limits_during_construction(self) -> None:
        self.assertFailure(
            "resource-bytes",
            lambda: self.checker.parse_canonical_fixture(
                b'{"a":0}', "lock", limits=(6, 64, 100)
            ),
        )
        self.assertFailure(
            "resource-depth",
            lambda: self.checker.parse_canonical_fixture(
                b"[[0]]", "lock", limits=(100, 2, 100)
            ),
        )
        self.assertFailure(
            "resource-values",
            lambda: self.checker.parse_canonical_fixture(
                b"[0,1]", "lock", limits=(100, 64, 2)
            ),
        )
        self.assertFailure(
            "uint64",
            lambda: self.checker.parse_canonical_fixture(
                b"9" * 5000, "lock", limits=(6000, 64, 10)
            ),
        )

    def test_lock_rejects_member_order_boolean_version_and_collection_order(self) -> None:
        reordered = deepcopy(self.lock)
        reordered["components"] = reordered.pop("components")
        # Move profiles after components without changing values.
        profiles = reordered.pop("profiles")
        reordered["profiles"] = profiles
        self.assertFailure("members", lambda: self.validate_lock(reordered))

        boolean_version = deepcopy(self.lock)
        boolean_version["schemaVersion"]["major"] = True
        self.assertFailure("version", lambda: self.validate_lock(boolean_version))

        unsorted_components = deepcopy(self.lock)
        unsorted_components["components"].reverse()
        self.assertFailure("order", lambda: self.validate_lock(unsorted_components))

        unsorted_capabilities = deepcopy(self.lock)
        build = unsorted_capabilities["components"][0]["profileBuilds"][0]
        build["capabilities"] = ["bloom.z", "bloom.a"]
        self.assertFailure("order", lambda: self.validate_lock(unsorted_capabilities))

    def test_lock_rejects_cross_reference_graph_and_ownership_failures(self) -> None:
        missing = deepcopy(self.lock)
        missing["components"][0]["dependencies"][0]["name"] = "component.missing"
        self.assertFailure("dependency-reference", lambda: self.validate_lock(missing))

        cycle = deepcopy(self.lock)
        cycle["components"][1]["dependencies"] = [
            {"name": "component.alpha", "relationship": "link"}
        ]
        self.assertFailure("dependency-cycle", lambda: self.validate_lock(cycle))

        duplicate_capability = deepcopy(self.lock)
        duplicate_capability["components"][1]["profileBuilds"][0]["capabilities"] = [
            "bloom.capability.alpha"
        ]
        self.assertFailure(
            "capability-owner", lambda: self.validate_lock(duplicate_capability)
        )

        unknown_profile = deepcopy(self.lock)
        unknown_profile["components"][1]["profileBuilds"][0]["profileId"] = (
            "bloom.profile.missing"
        )
        self.assertFailure("profile-reference", lambda: self.validate_lock(unknown_profile))

    def test_lock_rejects_fixture_claims_and_platform_mismatch(self) -> None:
        production_url = deepcopy(self.lock)
        production_url["components"][0]["source"]["url"] = "https://example.com/a.tar"
        self.assertFailure("fixture-url", lambda: self.validate_lock(production_url))

        outside_fixture = deepcopy(self.lock)
        outside_fixture["components"][0]["license"]["licenseFiles"][0]["path"] = (
            "dependencies/licenses/alpha/LICENSE"
        )
        self.assertFailure("fixture-separation", lambda: self.validate_lock(outside_fixture))

        invented_digest = deepcopy(self.lock)
        invented_digest["components"][0]["source"]["archiveSha256"] = "sha256:" + "0" * 64
        self.assertFailure("fixture-evidence", lambda: self.validate_lock(invented_digest))

        windows_sdk_on_linux = deepcopy(self.lock)
        windows_sdk_on_linux["profiles"][0]["consumerAbi"]["windowsSdk"] = "windows-sdk"
        self.assertFailure("abi-platform", lambda: self.validate_lock(windows_sdk_on_linux))

        windows_library_on_linux = deepcopy(self.lock)
        windows_library_on_linux["profiles"][0]["consumerAbi"]["standardLibrary"] = (
            "msvc-stl"
        )
        windows_library_on_linux["profiles"][0]["consumerAbi"][
            "libstdcxxCxx11Abi"
        ] = None
        self.assertFailure(
            "abi-platform", lambda: self.validate_lock(windows_library_on_linux)
        )

        windows_runtime_on_linux = deepcopy(self.lock)
        windows_runtime_on_linux["profiles"][0]["consumerAbi"]["cxxRuntime"] = "msvc"
        self.assertFailure(
            "abi-platform", lambda: self.validate_lock(windows_runtime_on_linux)
        )

        control_flag = deepcopy(self.lock)
        control_flag["profiles"][0]["consumerAbi"]["abiFlags"] = ["\0"]
        self.assertFailure("printable-token", lambda: self.validate_lock(control_flag))

        control_identity = deepcopy(self.lock)
        control_identity["profiles"][0]["consumerAbi"]["compilerAbi"] = "\0"
        self.assertFailure("ascii-identity", lambda: self.validate_lock(control_identity))

    def test_lock_binds_patch_and_unicode_paths_to_exact_fixture_bytes(self) -> None:
        swapped_unicode = deepcopy(self.lock)
        files = swapped_unicode["unicodeProfile"]["files"]
        files[0]["sha256"], files[1]["sha256"] = files[1]["sha256"], files[0]["sha256"]
        self.assertFailure("fixture-evidence", lambda: self.validate_lock(swapped_unicode))

        swapped_patch = deepcopy(self.lock)
        component = swapped_patch["components"][0]
        component["license"]["modified"] = True
        component["patches"] = [
            {
                "path": "dependencies/tests/fixtures/payloads/alpha-license.txt",
                "sha256": self.lock["components"][1]["license"]["licenseFiles"][0][
                    "sha256"
                ],
                "applyOrder": 0,
                "reason": "Synthetic path-to-digest mismatch",
            }
        ]
        self.assertFailure("fixture-evidence", lambda: self.validate_lock(swapped_patch))

    def test_prefix_rejects_identity_copy_and_capability_mismatch(self) -> None:
        wrong_lock = deepcopy(self.prefix)
        wrong_lock["lockIdentity"] = "sha256:" + "0" * 64
        self.assertFailure("lock-identity", lambda: self.validate_prefix(wrong_lock))

        wrong_profile = deepcopy(self.prefix)
        wrong_profile["profile"]["toolchain"]["compiler"]["identity"] = "changed"
        self.assertFailure("lock-copy", lambda: self.validate_prefix(wrong_profile))

        wrong_build = deepcopy(self.prefix)
        wrong_build["components"][0]["buildOptionsIdentity"] = "sha256:" + "0" * 64
        self.assertFailure("component-copy", lambda: self.validate_prefix(wrong_build))

        wrong_provider = deepcopy(self.prefix)
        wrong_provider["capabilities"][0]["providerComponent"] = "component.beta"
        self.assertFailure("capability-copy", lambda: self.validate_prefix(wrong_provider))

        boolean_version = deepcopy(self.prefix)
        boolean_version["schemaVersion"]["major"] = True
        self.assertFailure("version", lambda: self.validate_prefix(boolean_version))

        boolean_copy = deepcopy(self.prefix)
        boolean_copy["profile"]["consumerAbi"]["cxxStandard"] = True
        self.assertFailure("lock-copy", lambda: self.validate_prefix(boolean_copy))

    def test_prefix_hostile_shapes_produce_typed_diagnostics(self) -> None:
        cases = []
        malformed_component = deepcopy(self.prefix)
        del malformed_component["components"][0]["name"]
        cases.append(malformed_component)
        malformed_installed = deepcopy(self.prefix)
        del malformed_installed["installedFiles"][0]["path"]
        cases.append(malformed_installed)
        malformed_result = deepcopy(self.prefix)
        del malformed_result["qualificationResults"][0]["gateId"]
        cases.append(malformed_result)
        for value in cases:
            with self.subTest(root_members=list(value)):
                self.assertFailure("members", lambda value=value: self.validate_prefix(value))

    def test_prefix_rejects_installed_and_qualification_mismatch(self) -> None:
        wrong_tuple = deepcopy(self.prefix)
        wrong_tuple["installedFiles"][0]["permissions"] = "none"
        self.assertFailure("installed-tuple", lambda: self.validate_prefix(wrong_tuple))

        wrong_size = deepcopy(self.prefix)
        wrong_size["installedFiles"][0]["size"] += 1
        self.assertFailure("fixture-size", lambda: self.validate_prefix(wrong_size))

        missing_copy = deepcopy(self.prefix)
        missing_copy["installedFiles"] = missing_copy["installedFiles"][1:]
        self.assertFailure("evidence-copy", lambda: self.validate_prefix(missing_copy))

        failed_gate = deepcopy(self.prefix)
        failed_gate["qualificationResults"][0]["status"] = "failed"
        self.assertFailure("qualification", lambda: self.validate_prefix(failed_gate))

        wall_clock = deepcopy(self.prefix)
        wall_clock["qualificationResults"][0]["completedAt"] = "2000-01-01T00:00:00Z"
        self.assertFailure("qualification", lambda: self.validate_prefix(wall_clock))

        missing_evidence = deepcopy(self.prefix)
        missing_evidence["qualificationResults"][0]["evidence"][0]["path"] = (
            "share/bloom/dependencies/evidence/missing.txt"
        )
        self.assertFailure("gate-evidence", lambda: self.validate_prefix(missing_evidence))

    def test_prefix_rejects_collisions_prefix_conflicts_links_and_extra_roles(self) -> None:
        collision = deepcopy(self.prefix)
        collision["installedFiles"].extend(
            [self.extra_installed("extra/A"), self.extra_installed("extra/a")]
        )
        collision["installedFiles"].sort(key=lambda item: item["path"].encode())
        self.assertFailure("portable-collision", lambda: self.validate_prefix(collision))

        prefix_conflict = deepcopy(self.prefix)
        prefix_conflict["installedFiles"].extend(
            [
                self.extra_installed("extra/tree"),
                self.extra_installed("extra/tree/child"),
            ]
        )
        prefix_conflict["installedFiles"].sort(key=lambda item: item["path"].encode())
        self.assertFailure("path-prefix", lambda: self.validate_prefix(prefix_conflict))

        symbolic_link = deepcopy(self.prefix)
        link = symbolic_link["installedFiles"][0]
        link.update(
            {
                "type": "symbolic-link",
                "size": None,
                "sha256": None,
                "permissions": "none",
                "linkTarget": "target",
            }
        )
        self.assertFailure("fixture-symlink", lambda: self.validate_prefix(symbolic_link))

        extra_role = deepcopy(self.prefix)
        extra_role["installedFiles"].append(
            self.extra_installed(
                "zz/extra-library", component="component.alpha", role="library"
            )
        )
        extra_role["installedFiles"].sort(key=lambda item: item["path"].encode())
        self.assertFailure("shipping-role", lambda: self.validate_prefix(extra_role))

        prefix_wide_shipping = deepcopy(self.prefix)
        prefix_wide_shipping["installedFiles"].append(
            self.extra_installed("zz/prefix-library", role="library")
        )
        prefix_wide_shipping["installedFiles"].sort(
            key=lambda item: item["path"].encode()
        )
        self.assertFailure(
            "installed-component", lambda: self.validate_prefix(prefix_wide_shipping)
        )

    def test_prefix_rejects_non_ascii_cmake_target_bytes(self) -> None:
        value = deepcopy(self.prefix)
        config_path = "zz/synthetic-config.cmake"
        value["installedFiles"].append(
            self.extra_installed(
                config_path, component="component.alpha", role="cmake-package"
            )
        )
        value["installedFiles"].sort(key=lambda item: item["path"].encode())
        value["cmakePackages"] = [
            {
                "name": "SyntheticPackage",
                "version": "synthetic",
                "configPath": config_path,
                "targets": ["Bloom::Target\x7f"],
            }
        ]
        self.assertFailure("printable-token", lambda: self.validate_prefix(value))

    def test_timestamp_upper_bound_is_platform_independent(self) -> None:
        self.assertEqual(
            self.checker._timestamp_from_epoch(253_402_300_799),
            "9999-12-31T23:59:59Z",
        )

    def test_production_paths_cannot_resolve_to_fixtures_or_aliases(self) -> None:
        fixture_root = _ROOT / "dependencies" / "tests" / "fixtures"
        expected = _ROOT / "dependencies" / "dependencies.lock.json"
        self.assertFailure(
            "fixture-separation",
            lambda: self.checker.reject_fixture_as_production_path(
                fixture_root / "valid-lock.json", expected, fixture_root
            ),
        )
        self.assertFailure(
            "production-path",
            lambda: self.checker.reject_fixture_as_production_path(
                _ROOT / "dependencies" / "some-other-lock.json", expected, fixture_root
            ),
        )
        self.assertFailure(
            "production-path",
            lambda: self.checker.reject_fixture_as_production_path(
                _ROOT
                / "dependencies"
                / "alias"
                / ".."
                / "dependencies.lock.json",
                expected,
                fixture_root,
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected_path = root / "dependencies" / "dependencies.lock.json"
            expected_path.parent.mkdir(parents=True)
            expected_path.write_bytes(b"fixture")
            alias = root / "lock-alias.json"
            try:
                alias.symlink_to(expected_path)
            except OSError:
                pass
            else:
                self.assertFailure(
                    "production-path",
                    lambda: self.checker.reject_fixture_as_production_path(
                        alias, expected_path, root / "fixtures"
                    ),
                )

    def test_fixture_evidence_rejects_symlink_components(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload_root = root / "dependencies" / "tests" / "fixtures" / "payloads"
            payload_root.mkdir(parents=True)
            target = root / "target.txt"
            target.write_bytes(b"synthetic")
            link = payload_root / "link.txt"
            try:
                link.symlink_to(target)
            except OSError:
                self.skipTest("symlink creation is unavailable")
            context = {
                "root": root,
                "payload_root": payload_root.resolve(),
                "payload_digests": set(),
                "payload_sizes": {},
            }
            self.assertFailure(
                "fixture-evidence",
                lambda: self.checker._fixture_file(
                    "dependencies/tests/fixtures/payloads/link.txt",
                    "sha256:" + "0" * 64,
                    "fixture",
                    context,
                ),
            )

    def test_public_surface_cannot_be_mistaken_for_production_validation(self) -> None:
        self.assertIn("not a production lock/prefix validator", self.checker.__doc__)
        for name in self.checker.__all__:
            self.assertNotIn("identity", name)
        self.assertFalse(hasattr(self.checker, "lock_identity"))
        self.assertFalse(hasattr(self.checker, "prefix_identity"))


if __name__ == "__main__":
    unittest.main()
