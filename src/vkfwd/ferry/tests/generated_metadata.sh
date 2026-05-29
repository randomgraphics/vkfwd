#!/usr/bin/env sh
set -eu

root="${1:-.}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
core_tmp="$tmpdir/core"
forwarder_tmp="$tmpdir/forwarder"

"$root/src/vkfwd/ferry/scripts/generator/vulkan_metadata.py" \
  --output-dir "$core_tmp" \
  --forwarder-output-dir "$forwarder_tmp"

cmp "$root/src/vkfwd/ferry/core/generated/vulkan_manifest.json" "$core_tmp/vulkan_manifest.json"
cmp "$root/src/vkfwd/ferry/core/generated/vulkan_coverage.md" "$core_tmp/vulkan_coverage.md"
cmp "$root/src/vkfwd/ferry/core/generated/vulkan_api.hpp" "$core_tmp/vulkan_api.hpp"
cmp "$root/src/vkfwd/ferry/core/generated/vulkan_api.cpp" "$core_tmp/vulkan_api.cpp"
cmp "$root/src/vkfwd/ferry/core/generated/vulkan_manual_hooks.hpp" "$core_tmp/vulkan_manual_hooks.hpp"
cmp "$root/src/vkfwd/ferry/core/generated/README.md" "$core_tmp/README.md"
cmp "$root/src/vkfwd/ferry/forwarder/generated/vulkan_forwarder.hpp" "$forwarder_tmp/vulkan_forwarder.hpp"
cmp "$root/src/vkfwd/ferry/forwarder/generated/vulkan_forwarder.cpp" "$forwarder_tmp/vulkan_forwarder.cpp"
cmp "$root/src/vkfwd/ferry/forwarder/generated/vulkan_forwarder_hooks.hpp" "$forwarder_tmp/vulkan_forwarder_hooks.hpp"

for command in \
  vkCreateInstance \
  vkDestroyInstance \
  vkCreateDevice \
  vkDestroyDevice
do
  cmp "$root/src/vkfwd/ferry/core/generated/command/$command.metadata.json" "$core_tmp/command/$command.metadata.json"
  cmp "$root/src/vkfwd/ferry/core/generated/command/$command.hpp" "$core_tmp/command/$command.hpp"
  cmp "$root/src/vkfwd/ferry/core/generated/command/$command.cpp" "$core_tmp/command/$command.cpp"
  cmp "$root/src/vkfwd/ferry/forwarder/generated/command/$command.cpp" "$forwarder_tmp/command/$command.cpp"
done

python3 - \
  "$root/src/vkfwd/ferry/core/generated/vulkan_manifest.json" \
  "$root/src/vkfwd/ferry/core/generated/command/vkCreateInstance.metadata.json" \
  "$root/src/vkfwd/ferry/core/generated/command/vkCreateDevice.metadata.json" <<'PY'
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
assert manifest["commands"] == [
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkCreateDevice",
    "vkDestroyDevice",
]
assert commands["vkCreateInstance"]["level"] == "global"
assert commands["vkCreateDevice"]["level"] == "instance"
assert commands["vkCreateInstance"]["id"] == 2472334652
assert commands["vkCreateDevice"]["id"] == 1470473620
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
