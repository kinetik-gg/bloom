#!/usr/bin/env python3
"""Reject repository content that should never enter Bloom's public history."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, Sequence


_IGNORED_DIRECTORIES = {
    ".git",
    ".cache",
    ".idea",
    ".pytest_cache",
    ".vs",
    ".vscode",
    "__pycache__",
    "build",
}

_GENERATED_PATH_PARTS = {
    ".cache",
    ".idea",
    ".pytest_cache",
    ".vs",
    ".vscode",
    "CMakeFiles",
    "__pycache__",
    "build",
}

_GENERATED_FILENAMES = {
    ".DS_Store",
    "CMakeCache.txt",
    "Desktop.ini",
    "Thumbs.db",
    "cmake_install.cmake",
    "compile_commands.json",
}

_GENERATED_SUFFIXES = {
    ".a",
    ".autosave",
    ".class",
    ".dll",
    ".dylib",
    ".exe",
    ".ilk",
    ".lib",
    ".log",
    ".o",
    ".obj",
    ".pdb",
    ".pyc",
    ".pyo",
    ".so",
    ".suo",
    ".swp",
    ".user",
}

_SENSITIVE_FILENAMES = {
    ".env",
    ".netrc",
    "credentials",
    "credentials.json",
    "id_dsa",
    "id_ecdsa",
    "id_ed25519",
    "id_rsa",
}

_SENSITIVE_SUFFIXES = {".key", ".p12", ".pfx", ".pem"}

_TEXT_SUFFIXES = {
    ".bat",
    ".c",
    ".cc",
    ".cfg",
    ".cmake",
    ".conf",
    ".cpp",
    ".css",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ini",
    ".inl",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".qrc",
    ".rst",
    ".sh",
    ".toml",
    ".txt",
    ".ui",
    ".xml",
    ".yaml",
    ".yml",
}

_TEXT_FILENAMES = {
    ".clang-format",
    ".editorconfig",
    ".gitignore",
    "AGENTS.md",
    "CMakeLists.txt",
    "LICENSE",
    "NOTICE",
}

_VENDORED_DIRECTORY_NAMES = {"external", "third-party", "third_party", "vendor"}
_VENDORED_LICENSE_FILENAMES = {
    "COPYING",
    "COPYING.md",
    "COPYING.txt",
    "LICENSE",
    "LICENSE.md",
    "LICENSE.txt",
    "NOTICE",
    "NOTICE.md",
    "NOTICE.txt",
}

# These files necessarily contain the signatures exercised by the checker.
_CONTENT_SCAN_EXCLUSIONS = {
    "scripts/check_repository_hygiene.py",
    "tests/test_repository_checks.py",
}

_PLACEHOLDER_USERS = {
    "$user",
    "${user}",
    "%username%",
    "<user>",
    "example",
    "name",
    "user",
    "username",
}

_PLACEHOLDER_SECRETS = {
    "<secret>",
    "<token>",
    "changeme",
    "dummy",
    "example",
    "placeholder",
    "redacted",
    "test",
    "your-secret",
    "your-token",
}

_POSIX_USER_PATH = re.compile(r"/(?:home|Users)/(?P<user>[^/\s`\"'<>]+)/")
_WINDOWS_USER_PATH = re.compile(
    r"\b[A-Za-z]:[\\/]+Users[\\/]+(?P<user>[^\\/\s`\"'<>]+)[\\/]"
)

_CREDENTIAL_SIGNATURES: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "private key",
        re.compile(r"-----BEGIN (?:EC |OPENSSH |PGP |RSA )?PRIVATE KEY-----"),
    ),
    ("AWS access key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{24,}\b")),
    ("GitLab token", re.compile(r"\bglpat-[A-Za-z0-9_-]{20,}\b")),
    ("Slack token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b")),
    (
        "credential-bearing URL",
        re.compile(r"\b[a-z][a-z0-9+.-]*://[^\s/:@]+:[^\s/@]+@", re.IGNORECASE),
    ),
)

_QUOTED_SECRET_ASSIGNMENT = re.compile(
    r"\b(?:api[_-]?key|auth[_-]?token|password|passwd|secret|access[_-]?token)\b"
    r"\s*[:=]\s*['\"](?P<value>[^'\"]+)['\"]",
    re.IGNORECASE,
)

_YAML_SECRET_ASSIGNMENT = re.compile(
    r"^\s*(?:api[_-]?key|auth[_-]?token|password|passwd|secret|access[_-]?token)"
    r"\s*:\s*(?P<value>[^\s#]+)",
    re.IGNORECASE,
)

class Finding:
    def __init__(self, path: Path, category: str, message: str, line: int | None = None) -> None:
        self.path = path
        self.category = category
        self.message = message
        self.line = line

    def render(self) -> str:
        location = self.path.as_posix()
        if self.line is not None:
            location = f"{location}:{self.line}"
        return f"{location}: {self.category}: {self.message}"


def _git_repository_files(root: Path) -> list[Path] | None:
    try:
        result = subprocess.run(
            ["git", "-C", os.fspath(root), "ls-files", "--cached", "--others", "--exclude-standard"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

    return sorted(Path(line) for line in result.stdout.splitlines() if line)


def repository_files(root: Path) -> list[Path]:
    git_files = _git_repository_files(root)
    if git_files is not None:
        # `git ls-files --cached` intentionally reports tracked paths deleted in the working tree.
        # A legitimate rename/delete has no bytes to inspect and must not become a read failure.
        # `lexists` retains dangling symlinks so the path policy can still diagnose them.
        return [relative for relative in git_files if os.path.lexists(root / relative)]

    files: list[Path] = []
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if any(part in _IGNORED_DIRECTORIES for part in relative.parts):
            continue
        if path.is_file() or path.is_symlink():
            files.append(relative)
    return sorted(files)


def _is_placeholder_user(user: str) -> bool:
    return user.casefold() in _PLACEHOLDER_USERS


def _is_placeholder_secret(value: str) -> bool:
    normalized = value.strip("'\"").casefold()
    return (
        normalized in _PLACEHOLDER_SECRETS
        or normalized.startswith("${")
        or normalized.startswith("$env{")
        or (normalized.startswith("<") and normalized.endswith(">"))
        or normalized.startswith("your_")
        or normalized.startswith("your-")
        or set(normalized) <= {"x", "*"}
    )


def _is_text_file(relative: Path) -> bool:
    return relative.name in _TEXT_FILENAMES or relative.suffix.casefold() in _TEXT_SUFFIXES


def _path_findings(root: Path, relative: Path) -> Iterable[Finding]:
    parts = set(relative.parts)
    if parts & _GENERATED_PATH_PARTS:
        yield Finding(relative, "generated artifact", "generated/build directory must not be tracked")
    if relative.name in _GENERATED_FILENAMES or relative.suffix.casefold() in _GENERATED_SUFFIXES:
        yield Finding(relative, "generated artifact", "generated/build output must not be tracked")
    if relative.name in _SENSITIVE_FILENAMES or relative.suffix.casefold() in _SENSITIVE_SUFFIXES:
        yield Finding(relative, "sensitive file", "credential or private-key file must not be published")

    absolute = root / relative
    if absolute.is_symlink():
        target = Path(os.readlink(absolute))
        resolved = target if target.is_absolute() else (absolute.parent / target).resolve()
        try:
            resolved.relative_to(root.resolve())
        except ValueError:
            yield Finding(relative, "unsafe symlink", "symlink resolves outside the repository")


def _content_findings(relative: Path, text: str) -> Iterable[Finding]:
    for line_number, line in enumerate(text.splitlines(), start=1):
        for matcher in (_POSIX_USER_PATH, _WINDOWS_USER_PATH):
            for match in matcher.finditer(line):
                if not _is_placeholder_user(match.group("user")):
                    yield Finding(
                        relative,
                        "machine-specific path",
                        f"absolute user path contains account name {match.group('user')!r}",
                        line_number,
                    )

        for label, matcher in _CREDENTIAL_SIGNATURES:
            if matcher.search(line):
                yield Finding(relative, "credential", f"possible {label}", line_number)

        for matcher in (_QUOTED_SECRET_ASSIGNMENT, _YAML_SECRET_ASSIGNMENT):
            match = matcher.search(line)
            if match and not _is_placeholder_secret(match.group("value")):
                yield Finding(
                    relative,
                    "credential",
                    "secret-like assignment contains a non-placeholder value",
                    line_number,
                )

def _repository_policy_findings(root: Path, candidates: Sequence[Path]) -> Iterable[Finding]:
    license_path = root / "LICENSE"
    notice_path = root / "NOTICE"

    if not license_path.is_file():
        yield Finding(Path("LICENSE"), "license policy", "root Apache-2.0 license is missing")
    else:
        license_text = license_path.read_text(encoding="utf-8", errors="replace")
        if "Apache License" not in license_text or "Version 2.0" not in license_text:
            yield Finding(
                Path("LICENSE"),
                "license policy",
                "root license does not identify the Apache License, Version 2.0",
            )

    if not notice_path.is_file():
        yield Finding(Path("NOTICE"), "license policy", "root NOTICE file is missing")

    vendored_components: set[Path] = set()
    for relative in candidates:
        parts = relative.parts
        for index, part in enumerate(parts[:-1]):
            if part.casefold() in _VENDORED_DIRECTORY_NAMES and index + 1 < len(parts) - 1:
                vendored_components.add(Path(*parts[: index + 2]))
                break

    if not vendored_components:
        return

    inventory = root / "THIRD_PARTY_NOTICES.md"
    if not inventory.is_file():
        yield Finding(
            Path("THIRD_PARTY_NOTICES.md"),
            "third-party attribution",
            "vendored content requires a repository-level attribution inventory",
        )

    candidate_set = set(candidates)
    for component in sorted(vendored_components):
        if not any(component / filename in candidate_set for filename in _VENDORED_LICENSE_FILENAMES):
            yield Finding(
                component,
                "third-party attribution",
                "vendored component must retain an upstream license or notice file",
            )


def scan_repository(root: Path, files: Sequence[Path] | None = None) -> list[Finding]:
    root = root.resolve()
    candidates = repository_files(root) if files is None else sorted(files)
    findings: list[Finding] = []

    findings.extend(_repository_policy_findings(root, candidates))

    for relative in candidates:
        findings.extend(_path_findings(root, relative))
        if relative.as_posix() in _CONTENT_SCAN_EXCLUSIONS or not _is_text_file(relative):
            continue

        absolute = root / relative
        try:
            if absolute.stat().st_size > 2 * 1024 * 1024:
                continue
            text = absolute.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            findings.append(Finding(relative, "read failure", str(error)))
            continue
        findings.extend(_content_findings(relative, text))

    return findings


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    findings = scan_repository(args.root)
    if findings:
        print(f"Repository hygiene failed with {len(findings)} finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding.render()}", file=sys.stderr)
        return 1

    print("Repository hygiene passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
