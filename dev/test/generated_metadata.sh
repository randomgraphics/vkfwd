#!/usr/bin/env sh
set -eu

root="${1:-.}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

"$root/dev/generator/vulkan_metadata.py" --output-dir "$tmpdir"

cmp "$root/src/vkfwd/generated/vulkan_metadata.json" "$tmpdir/vulkan_metadata.json"
cmp "$root/src/vkfwd/generated/vulkan_coverage.md" "$tmpdir/vulkan_coverage.md"
cmp "$root/src/vkfwd/generated/vulkan_api.hpp" "$tmpdir/vulkan_api.hpp"
cmp "$root/src/vkfwd/generated/vulkan_api.cpp" "$tmpdir/vulkan_api.cpp"
cmp "$root/src/vkfwd/generated/vulkan_manual_hooks.hpp" "$tmpdir/vulkan_manual_hooks.hpp"
cmp "$root/src/vkfwd/generated/README.md" "$tmpdir/README.md"
cmp "$root/src/vkfwd/generated/commands/vkCreateInstance.hpp" "$tmpdir/commands/vkCreateInstance.hpp"
cmp "$root/src/vkfwd/generated/commands/vkCreateInstance.cpp" "$tmpdir/commands/vkCreateInstance.cpp"
cmp "$root/src/vkfwd/generated/commands/vkCreateDevice.hpp" "$tmpdir/commands/vkCreateDevice.hpp"
cmp "$root/src/vkfwd/generated/commands/vkCreateDevice.cpp" "$tmpdir/commands/vkCreateDevice.cpp"

python3 - "$root/src/vkfwd/generated/vulkan_metadata.json" <<'PY'
import json
import sys

metadata_path = sys.argv[1]
with open(metadata_path, encoding="utf-8") as handle:
    metadata = json.load(handle)

commands = {command["name"]: command for command in metadata["commands"]}
assert set(commands) == {"vkCreateInstance", "vkCreateDevice"}
assert commands["vkCreateInstance"]["level"] == "global"
assert commands["vkCreateDevice"]["level"] == "instance"
assert commands["vkCreateInstance"]["creates_handles"] == ["VkInstance"]
assert commands["vkCreateDevice"]["creates_handles"] == ["VkDevice"]
assert commands["vkCreateDevice"]["dispatch_parameter"] == "physicalDevice"
assert metadata["protocol"]["wire_major"] == 1
assert metadata["protocol"]["wire_minor"] == 0
assert metadata["protocol"]["generator_schema_version"] == 1
assert metadata["versions"]["vulkan_api"] == {"major": 1, "minor": 4, "patch": 352}
assert metadata["handles"]["VkDevice"]["parent"] == "VkPhysicalDevice"
assert metadata["structs"]["VkInstanceCreateInfo"]["has_pnext"] is True
assert metadata["structs"]["VkDeviceCreateInfo"]["has_pnext"] is True
PY
