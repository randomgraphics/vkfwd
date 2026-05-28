#!/usr/bin/env sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

export VKFWD_ROOT="$repo_root"
export VK_LAYER_PATH="$repo_root/build/src/vkfwd${VK_LAYER_PATH:+:$VK_LAYER_PATH}"

printf 'VKFWD_ROOT=%s\n' "$VKFWD_ROOT"
printf 'VK_LAYER_PATH=%s\n' "$VK_LAYER_PATH"
