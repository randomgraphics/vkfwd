#!/usr/bin/env sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
build_dir="${VKFWD_BUILD_DIR:-$repo_root/build/linux.gcc.debug}"

"$repo_root/dev/bin/build.py" d
ctest --test-dir "$build_dir" --output-on-failure
