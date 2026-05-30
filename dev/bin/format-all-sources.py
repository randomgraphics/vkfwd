#!/usr/bin/env python3

import argparse
import concurrent.futures
import os
import platform
import subprocess
import sys
import threading
from pathlib import Path

SOURCE_PATTERNS = [
    "*.h",
    "*.hpp",
    "*.hh",
    "*.hxx",
    "*.inl",
    "*.c",
    "*.cc",
    "*.cpp",
    "*.cxx",
    "*.m",
    "*.mm",
    "*.java",
    "*.glsl",
    "*.frag",
    "*.vert",
    "*.comp",
    "*.py",
]

# Formatting should cover vkfwd-owned source only. git ls-files already limits
# traversal to tracked files, so this only protects vendored source boundaries.
EXCLUDED_PATH_PARTS = {
    "third_party",
    "3rdparty",
    "3rd-party",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-n",
        action="store_true",
        help="Dry run. Print format violations to stderr, if any.",
    )
    parser.add_argument("-q", action="store_true", help="Quiet mode. Mute stdout.")
    parser.add_argument(
        "-d",
        action="store_true",
        help="Only process files that are different than the main branch.",
    )
    parser.add_argument(
        "--black",
        default=os.environ.get("BLACK", "black"),
        help="black executable. Defaults to BLACK or 'black'.",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="Optional tracked file or directory scopes to format.",
    )
    return parser.parse_args()


def normalize_scope(root: Path, path: str) -> str:
    candidate = Path(path)
    absolute = candidate if candidate.is_absolute() else root / candidate
    try:
        relative = absolute.resolve().relative_to(root.resolve())
    except ValueError:
        raise SystemExit(f"Error: format scope is outside repository: {path}")
    if relative == Path("."):
        return ""
    return relative.as_posix()


def tracked_sources(root: Path, changed_only: bool, paths: list[str]) -> list[str]:
    scopes = [normalize_scope(root, path) for path in paths]
    if changed_only:
        base = diff_base(root)
        command = ["git", "diff", "--name-only", base, "--", *SOURCE_PATTERNS]
    else:
        command = ["git", "ls-files", *SOURCE_PATTERNS]
    sources = subprocess.check_output(command, cwd=root).decode("utf-8").splitlines()
    return [path for path in sources if is_in_scope(path, scopes)]


def is_in_scope(path: str, scopes: list[str]) -> bool:
    if not scopes:
        return True
    # Scopes deliberately filter Git's tracked-file result instead of replacing
    # it, so generators cannot hide newly emitted files from the normal review
    # and git-add step by formatting untracked output implicitly.
    return any(
        not scope or path == scope or path.startswith(f"{scope.rstrip('/')}/")
        for scope in scopes
    )


def diff_base(root: Path) -> str:
    for candidate in ("origin/main", "origin/master", "main", "master"):
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", candidate],
            cwd=root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            return candidate
    return "HEAD"


def is_owned_source(path: str) -> bool:
    return not any(part in EXCLUDED_PATH_PARTS for part in Path(path).parts)


def find_clang_format(root: Path) -> str:
    system = platform.system()
    if system == "Windows":
        candidate = root / "dev/bin/clang-format/clang-format-22.1.0.exe"
    elif system == "Darwin":
        candidate = root / "dev/bin/clang-format/clang-format-22.1.0-apple"
    else:
        candidate = root / "dev/bin/clang-format/clang-format-22.1.0-x64-linux"

    if candidate.is_file():
        return str(candidate)

    print(
        f"Error: checked-in clang-format not found: {candidate}",
        file=sys.stderr,
    )
    sys.exit(1)


def require_executable(name: str, description: str) -> str:
    try:
        resolved = subprocess.run(
            [name, "--version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if resolved.returncode == 0:
            return name
    except FileNotFoundError:
        pass
    print(f"Error: {description} not found: {name}", file=sys.stderr)
    sys.exit(1)


def run_formatter(
    command: list[str], root: Path, quiet: bool, lock: threading.Lock
) -> None:
    if not quiet:
        with lock:
            print(" ".join(command))
    result = subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )
    if result.stdout.strip() and not quiet:
        with lock:
            print(result.stdout.strip())
    if result.stderr.strip():
        with lock:
            sys.stderr.write(result.stderr.strip())
            sys.stderr.write("\n")


def format_one_file(
    path: str,
    root: Path,
    clang_format: str,
    black: str,
    dry_run: bool,
    quiet: bool,
    lock: threading.Lock,
) -> None:
    if path.endswith(".py"):
        command = [black, "--check" if dry_run else path]
        if dry_run:
            command.append(path)
        run_black(command, root, quiet, lock)
        return

    command = [clang_format, "--dry-run" if dry_run else "-i", path]
    run_formatter(command, root, quiet, lock)


def run_black(
    command: list[str], root: Path, quiet: bool, lock: threading.Lock
) -> None:
    if not quiet:
        with lock:
            print(" ".join(command))
    result = subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )
    if result.stdout.strip() and not quiet:
        with lock:
            print(result.stdout.strip())

    # Black writes normal summaries to stderr. Keep failure diagnostics visible,
    # but avoid noisy "all done" output when the file is already formatted.
    err = result.stderr.strip()
    is_informational = (
        err.startswith("All done!") or "file would be left unchanged" in err
    )
    if err and not is_informational:
        with lock:
            sys.stderr.write(err)
            sys.stderr.write("\n")


def main() -> int:
    args = parse_args()
    root = repo_root()
    sources = [
        path
        for path in tracked_sources(root, args.d, args.paths)
        if is_owned_source(path)
    ]
    has_python = any(path.endswith(".py") for path in sources)
    has_non_python = any(not path.endswith(".py") for path in sources)
    clang_format = find_clang_format(root) if has_non_python else ""
    black = require_executable(args.black, "black") if has_python else ""
    lock = threading.Lock()

    with concurrent.futures.ThreadPoolExecutor() as executor:
        futures = {
            executor.submit(
                format_one_file,
                path,
                root,
                clang_format,
                black,
                args.n,
                args.q,
                lock,
            ): path
            for path in sources
        }
        for future in concurrent.futures.as_completed(futures):
            future.result()

    return 0


if __name__ == "__main__":
    sys.exit(main())
