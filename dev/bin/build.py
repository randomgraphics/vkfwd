#!/usr/bin/env python3

import argparse
import configparser
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


VARIANTS = {
    "d": ("debug", "Debug"),
    "debug": ("debug", "Debug"),
    "r": ("release", "Release"),
    "release": ("release", "Release"),
    "released": ("release", "Release"),
    "p": ("profiling", "RelWithDebInfo"),
    "profile": ("profiling", "RelWithDebInfo"),
    "profiling": ("profiling", "RelWithDebInfo"),
}


class Submodule:
    def __init__(self, path: Path, url: str, branch: str | None):
        self.path = path
        self.url = url
        self.branch = branch


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def fail(message: str, exit_code: int = 1) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(exit_code)


def run(command: list[str], cwd: Path | None = None) -> None:
    print(" ".join(str(x) for x in command), flush=True)
    try:
        subprocess.check_call(command, cwd=cwd)
    except subprocess.CalledProcessError as err:
        sys.exit(err.returncode)


def submodules() -> list[Submodule]:
    gitmodules = repo_root() / ".gitmodules"
    if not gitmodules.is_file():
        return []

    config = configparser.ConfigParser()
    config.read(gitmodules)

    result: list[Submodule] = []
    for section in config.sections():
        if not section.startswith("submodule "):
            continue
        if not config.has_option(section, "path") or not config.has_option(section, "url"):
            continue
        branch = config.get(section, "branch") if config.has_option(section, "branch") else None
        result.append(
            Submodule(
                repo_root() / config.get(section, "path"),
                config.get(section, "url"),
                branch,
            )
        )
    return result


def submodule_needs_fetch(path: Path) -> bool:
    if not path.exists():
        return True
    if not path.is_dir():
        fail(f"submodule path exists but is not a directory: {path}")

    # A populated submodule has either a .git file that points at the real gitdir
    # or a .git directory for manually cloned checkouts. Empty directories are
    # treated as not fetched so CMake never configures against half state.
    if (path / ".git").exists():
        return False
    return not any(path.iterdir())


def fetch_missing_submodules() -> None:
    specs = submodules()
    missing = [spec for spec in specs if submodule_needs_fetch(spec.path)]
    if not missing:
        return

    print("Fetching missing git submodules:", flush=True)
    for spec in missing:
        print(f"  {spec.path.relative_to(repo_root())}", flush=True)

    # --recursive keeps nested third-party dependencies coherent, while --depth
    # preserves the repository policy that vendored dependencies should be
    # shallow unless a developer explicitly asks git for more history. In normal
    # checkouts this command follows the gitlinks recorded by the repository.
    run(
        [
            "git",
            "submodule",
            "update",
            "--init",
            "--recursive",
            "--depth",
            "1",
        ],
        cwd=repo_root(),
    )

    # During local bootstrapping, .gitmodules may exist before gitlinks are
    # registered. Fall back to the same shallow clone policy so build.py still
    # brings the dependency tree into a usable state from repository metadata.
    for spec in specs:
        if not submodule_needs_fetch(spec.path):
            continue
        spec.path.parent.mkdir(parents=True, exist_ok=True)
        command = [
            "git",
            "clone",
            "--depth",
            "1",
            "--recurse-submodules",
            "--shallow-submodules",
        ]
        if spec.branch:
            command.extend(["--branch", spec.branch])
        command.extend([spec.url, str(spec.path)])
        run(command, cwd=repo_root())


def existing_directory_from_env(*names: str) -> Path:
    for name in names:
        value = os.environ.get(name)
        if value:
            path = Path(value).expanduser()
            if path.is_dir():
                return path
            fail(f"{name} points to a missing folder: {path}")
    fail(f"missing environment variable: {' or '.join(names)}")


def host_platform_name() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "macos"
    if system == "windows":
        return "windows"
    return system


def compiler_name(args: argparse.Namespace) -> str:
    if args.android:
        # Android NDK builds use Clang regardless of the host compiler.
        return "clang"
    if args.clang:
        return "clang"
    if platform.system() == "Darwin":
        return "clang"
    if platform.system() == "Windows":
        return "msvc"
    return "gcc"


def target_platform_name(args: argparse.Namespace) -> str:
    if args.android:
        return f"android-{args.android_abi}"
    return host_platform_name()


def build_directory(args: argparse.Namespace, variant_name: str) -> Path:
    base = Path(args.build_root)
    if not base.is_absolute():
        base = repo_root() / base

    # Keep platform, compiler, and variant in the path so host and Android
    # configure state never overwrite each other's CMake cache.
    leaf = f"{target_platform_name(args)}.{compiler_name(args)}.{variant_name}"
    return base / leaf


def cmake_configure_args(args: argparse.Namespace, build_type: str, build_dir: Path) -> list[str]:
    root = repo_root()
    command = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]

    if args.android:
        sdk = existing_directory_from_env("ANDROID_SDK_ROOT", "ANDROID_HOME")
        ndk = existing_directory_from_env("ANDROID_NDK_ROOT", "ANDROID_NDK_HOME")
        toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
        if not toolchain.is_file():
            fail(f"Android CMake toolchain not found: {toolchain}")

        # CMake's Android generator state is tied to the ABI and API level, so
        # these values are explicit instead of relying on ambient SDK defaults.
        command.extend(
            [
                "-GNinja",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                f"-DANDROID_NDK={ndk}",
                f"-DCMAKE_ANDROID_NDK={ndk}",
                f"-DANDROID_ABI={args.android_abi}",
                f"-DCMAKE_ANDROID_ARCH_ABI={args.android_abi}",
                f"-DANDROID_PLATFORM=android-{args.android_api}",
                f"-DANDROID_NATIVE_API_LEVEL={args.android_api}",
                f"-DCMAKE_SYSTEM_VERSION={args.android_api}",
            ]
        )
        if platform.system() == "Windows":
            ninja_candidates = list((sdk / "cmake").glob("**/ninja.exe"))
            if not ninja_candidates:
                fail("ninja.exe not found in Android SDK. Install CMake from Android SDK Manager.")
            command.append(f"-DCMAKE_MAKE_PROGRAM={ninja_candidates[0]}")
    else:
        if args.ninja:
            command.append("-GNinja")
        if args.clang:
            command.extend(["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"])

    command.extend(args.cmake_args)
    return command


def clean_build_root(build_root: str) -> None:
    root = repo_root()
    base = Path(build_root)
    if not base.is_absolute():
        base = root / base
    if not base.exists():
        print(f"{base} does not exist.")
        return
    if base.resolve() == root.resolve():
        fail("refusing to clean the repository root")
    for child in sorted(base.iterdir()):
        print(f"rm {child}")
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Configure and build vkfwd.")
    parser.add_argument("variant", help="d/debug, r/release, p/profiling, or c/clean")
    parser.add_argument("--android", action="store_true", help="build for Android")
    parser.add_argument("--android-abi", default="arm64-v8a", help="Android ABI, default: arm64-v8a")
    parser.add_argument("--android-api", default="29", help="Android API level, default: 29")
    parser.add_argument("--build-root", default="build", help="root folder for build outputs")
    parser.add_argument("--clang", action="store_true", help="use Clang for host builds")
    parser.add_argument("--ninja", action="store_true", help="use Ninja for host builds")
    parser.add_argument("--configure-only", action="store_true", help="run CMake configure but skip build")
    parser.add_argument("--build-only", action="store_true", help="skip configure and build the existing tree")
    parser.add_argument("-j", "--jobs", default="8", help="parallel build jobs, default: 8")
    args, cmake_args = parser.parse_known_args()
    # Unknown options are intentionally forwarded to CMake configure so this
    # wrapper does not need to model every project-specific cache variable.
    args.cmake_args = cmake_args[1:] if cmake_args[:1] == ["--"] else cmake_args
    return args


def main() -> None:
    args = parse_args()
    variant_key = args.variant.lower()
    if variant_key in ("c", "clean", "cleanup"):
        clean_build_root(args.build_root)
        return

    if variant_key not in VARIANTS:
        fail(f"unrecognized build variant: {args.variant}")

    variant_name, cmake_build_type = VARIANTS[variant_key]
    build_dir = build_directory(args, variant_name)
    build_dir.mkdir(parents=True, exist_ok=True)

    print(f"Repository = {repo_root()}", flush=True)
    print(f"Build dir  = {build_dir}", flush=True)
    print(f"Variant    = {cmake_build_type}", flush=True)

    if not args.build_only:
        fetch_missing_submodules()
        run(cmake_configure_args(args, cmake_build_type, build_dir))
    if not args.configure_only:
        run(["cmake", "--build", str(build_dir), "--config", cmake_build_type, f"-j{args.jobs}"])


if __name__ == "__main__":
    main()
