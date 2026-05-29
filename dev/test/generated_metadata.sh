#!/usr/bin/env sh
set -eu

root="${1:-.}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

"$root/dev/generator/vulkan_metadata.py" --output-dir "$tmpdir"

cmp "$root/src/vkfwd/core/generated/vulkan_manifest.json" "$tmpdir/vulkan_manifest.json"
cmp "$root/src/vkfwd/core/generated/vulkan_coverage.md" "$tmpdir/vulkan_coverage.md"
cmp "$root/src/vkfwd/core/generated/vulkan_api.hpp" "$tmpdir/vulkan_api.hpp"
cmp "$root/src/vkfwd/core/generated/vulkan_api.cpp" "$tmpdir/vulkan_api.cpp"
cmp "$root/src/vkfwd/core/generated/vulkan_manual_hooks.hpp" "$tmpdir/vulkan_manual_hooks.hpp"
cmp "$root/src/vkfwd/core/generated/README.md" "$tmpdir/README.md"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateInstance.metadata.json" "$tmpdir/commands/vkCreateInstance.metadata.json"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateInstance.hpp" "$tmpdir/commands/vkCreateInstance.hpp"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateInstance.cpp" "$tmpdir/commands/vkCreateInstance.cpp"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateDevice.metadata.json" "$tmpdir/commands/vkCreateDevice.metadata.json"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateDevice.hpp" "$tmpdir/commands/vkCreateDevice.hpp"
cmp "$root/src/vkfwd/core/generated/commands/vkCreateDevice.cpp" "$tmpdir/commands/vkCreateDevice.cpp"

python3 - \
  "$root/src/vkfwd/core/generated/vulkan_manifest.json" \
  "$root/src/vkfwd/core/generated/commands/vkCreateInstance.metadata.json" \
  "$root/src/vkfwd/core/generated/commands/vkCreateDevice.metadata.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
with open(sys.argv[2], encoding="utf-8") as handle:
    create_instance = json.load(handle)
with open(sys.argv[3], encoding="utf-8") as handle:
    create_device = json.load(handle)

commands = {
    create_instance["command"]["name"]: create_instance["command"],
    create_device["command"]["name"]: create_device["command"],
}
assert manifest["commands"] == ["vkCreateInstance", "vkCreateDevice"]
assert set(commands) == {"vkCreateInstance", "vkCreateDevice"}
assert commands["vkCreateInstance"]["level"] == "global"
assert commands["vkCreateDevice"]["level"] == "instance"
assert commands["vkCreateInstance"]["creates_handles"] == ["VkInstance"]
assert commands["vkCreateDevice"]["creates_handles"] == ["VkDevice"]
assert commands["vkCreateDevice"]["dispatch_parameter"] == "physicalDevice"
assert manifest["protocol"]["wire_major"] == 1
assert manifest["protocol"]["wire_minor"] == 0
assert manifest["protocol"]["generator_schema_version"] == 1
assert manifest["versions"]["vulkan_api"] == {"major": 1, "minor": 4, "patch": 352}
assert create_device["handles"]["VkDevice"]["parent"] == "VkPhysicalDevice"
assert create_instance["structs"]["VkInstanceCreateInfo"]["has_pnext"] is True
assert create_device["structs"]["VkDeviceCreateInfo"]["has_pnext"] is True
PY
