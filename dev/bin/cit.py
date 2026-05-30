#!/usr/bin/env python3

import argparse
import platform
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_build_dir() -> Path:
    return repo_root() / "build" / "linux.gcc.debug"


def executable_suffix() -> str:
    return ".exe" if platform.system() == "Windows" else ""


def latest_internal_test_executable(build_dir: Path) -> Path | None:
    executable_name = f"vkfwd_internal_tests{executable_suffix()}"
    candidates = [path for path in build_dir.rglob(executable_name) if path.is_file()]
    if not candidates:
        return None

    # Multi-config generators may leave Debug/Release binaries side by side.
    # Choosing the newest file keeps cit.py aligned with the configuration that
    # was just built instead of baking in a generator-specific output path.
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def run(command: list[str], cwd: Path | None = None) -> int:
    print(" ".join(str(x) for x in command), flush=True)
    return subprocess.call(command, cwd=cwd)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the newest built vkfwd internal test binary.")
    parser.add_argument("--build-dir", default=default_build_dir(), type=Path, help="CMake build directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = repo_root()
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir

    # Run the Catch2 executable directly so cit.py reports the internal test
    # process status instead of CTest's wrapper status or output policy.
    test_exe = latest_internal_test_executable(build_dir)
    if test_exe is None:
        print(f"ERROR: vkfwd_internal_tests was not found under {build_dir}", file=sys.stderr)
        return 1
    return run([str(test_exe)], cwd=root)


if __name__ == "__main__":
    sys.exit(main())
