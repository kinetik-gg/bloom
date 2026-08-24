#!/usr/bin/env python3
"""Check inexpensive source-boundary invariants documented for Bloom."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, Sequence


_CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}
_QT_TOKEN = re.compile(r"(?:#\s*include\s*[<\"]Q[A-Z]|\bQt::|\bQ[A-Z][A-Za-z0-9_]*\b)")
_FORBIDDEN_SOURCE_INCLUDE = re.compile(
    r"#\s*include\s*[<\"](?:\.\.[/\\]|apps[/\\]|tests[/\\]|src[/\\])"
)


class Finding:
    def __init__(self, path: Path, line: int, message: str) -> None:
        self.path = path
        self.line = line
        self.message = message

    def render(self) -> str:
        return f"{self.path.as_posix()}:{self.line}: architecture: {self.message}"


def _repository_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return sorted(
            path.relative_to(root)
            for path in root.rglob("*")
            if path.is_file() and ".git" not in path.relative_to(root).parts
        )
    return sorted(Path(line) for line in result.stdout.splitlines() if line)


def _lines(path: Path) -> Iterable[tuple[int, str]]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    yield from enumerate(text.splitlines(), start=1)


def _is_non_ui_source(relative: Path) -> bool:
    return len(relative.parts) >= 2 and relative.parts[0] == "src" and relative.parts[1] != "ui"


def _check_public_header_path(relative: Path) -> Finding | None:
    parts = relative.parts
    if len(parts) < 4 or parts[0] != "src" or parts[2] != "include":
        return None
    module = parts[1]
    expected_prefix = ("src", module, "include", "bloom", module)
    if parts[:5] != expected_prefix:
        return Finding(
            relative,
            1,
            f"public header must live below src/{module}/include/bloom/{module}",
        )
    return None


def scan_repository(root: Path, files: Sequence[Path] | None = None) -> list[Finding]:
    root = root.resolve()
    candidates = _repository_files(root) if files is None else sorted(files)
    findings: list[Finding] = []

    for relative in candidates:
        if relative.suffix.casefold() in _CPP_SUFFIXES:
            public_path_finding = _check_public_header_path(relative)
            if public_path_finding is not None:
                findings.append(public_path_finding)

            for line_number, line in _lines(root / relative):
                if _is_non_ui_source(relative) and _QT_TOKEN.search(line):
                    findings.append(
                        Finding(relative, line_number, "Qt types are restricted to src/ui and apps")
                    )
                if relative.parts and relative.parts[0] == "src" and _FORBIDDEN_SOURCE_INCLUDE.search(line):
                    findings.append(
                        Finding(
                            relative,
                            line_number,
                            "src modules must not include apps/tests or bypass public include roots",
                        )
                    )

        if (
            relative.name == "CMakeLists.txt"
            and len(relative.parts) >= 3
            and relative.parts[0] == "src"
            and relative.parts[1] != "ui"
        ):
            for line_number, line in _lines(root / relative):
                if "Qt6::" in line:
                    findings.append(
                        Finding(relative, line_number, "non-UI source module links directly to Qt")
                    )

    return findings


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    findings = scan_repository(args.root)
    if findings:
        print(f"Architecture boundary check failed with {len(findings)} finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding.render()}", file=sys.stderr)
        return 1

    print("Architecture boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
