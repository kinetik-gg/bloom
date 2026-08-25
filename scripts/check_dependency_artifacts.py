#!/usr/bin/env python3
"""Check exact dependency schemas and synthetic fixtures, never production artifacts."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Sequence

from dependency_artifact_canonical import DependencyArtifactError
from dependency_artifact_validation import check_repository


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        lock_id, prefix_id = check_repository(args.root)
    except DependencyArtifactError as error:
        print(f"Dependency artifact check failed: {error}", file=sys.stderr)
        return 1
    print(f"Synthetic dependency contract check passed (lock {lock_id}, prefix {prefix_id})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
