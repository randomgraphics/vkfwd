#!/usr/bin/env python3
"""Generate the first vkfwd Vulkan code and metadata slice from the pinned registry."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

TARGET_COMMANDS = (
    "vkEnumerateInstanceVersion",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateInstanceExtensionProperties",
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkEnumeratePhysicalDevices",
    "vkGetPhysicalDeviceProperties",
    "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkEnumerateDeviceExtensionProperties",
    "vkCreateDevice",
    "vkDestroyDevice",
    "vkGetDeviceQueue",
    "vkDeviceWaitIdle",
    "vkCreateBuffer",
    "vkDestroyBuffer",
    "vkGetBufferMemoryRequirements",
    "vkAllocateMemory",
    "vkFreeMemory",
    "vkBindBufferMemory",
    "vkMapMemory",
    "vkUnmapMemory",
    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout",
    "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout",
    "vkCreateRenderPass",
    "vkDestroyRenderPass",
    "vkCreateGraphicsPipelines",
    "vkDestroyPipeline",
    "vkCreateSemaphore",
    "vkDestroySemaphore",
)
SUPPORTED_COMMAND_STRUCT_PARAMETERS = {
    "VkInstanceCreateInfo",
    "VkDeviceCreateInfo",
    "VkBufferCreateInfo",
    "VkMemoryAllocateInfo",
    "VkShaderModuleCreateInfo",
    "VkDescriptorSetLayoutCreateInfo",
    "VkPipelineLayoutCreateInfo",
    "VkRenderPassCreateInfo",
    "VkGraphicsPipelineCreateInfo",
    "VkSemaphoreCreateInfo",
}
GENERATOR_VERSION = "vkfwd-vulkan-metadata-0.1"
SCHEMA_VERSION = 1
COMMAND_REVISION = 1
# v2 of the id scheme constrains the hash output to stay strictly below
# RESERVED_COMMAND_ID_BASE so the upper region is permanently available to
# vkfwd-owned manual command ids. See src/vkfwd/ferry/core/command_id_range.hpp.
COMMAND_ID_SALT = "vkfwd.vulkan.command-id.v2:"
# Must match ::vkfwd::kReservedCommandIdBase in core/command_id_range.hpp.
RESERVED_COMMAND_ID_BASE = 0xFFFE0000


def repo_root() -> Path:
    return Path(__file__).resolve().parents[5]


def text_of(element: ET.Element | None) -> str:
    if element is None:
        return ""
    return "".join(element.itertext()).strip()


def declaration_of(element: ET.Element) -> str:
    pieces = [element.text or ""]
    for child in element:
        if child.tag != "comment":
            pieces.append(text_of(child))
        pieces.append(child.tail or "")
    return " ".join("".join(pieces).split())


def split_csv(value: str | None) -> list[str]:
    if not value:
        return []
    return [item for item in value.split(",") if item]


def parse_version_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if ": " not in line:
            continue
        key, value = line.split(": ", 1)
        values[key.lower().replace(" ", "_")] = value
    return values


def parse_semver(value: str | None) -> dict[str, int]:
    if not value:
        return {"major": 0, "minor": 0, "patch": 0}
    parts = (value.split(".") + ["0", "0", "0"])[:3]
    return {
        "major": int(parts[0]),
        "minor": int(parts[1]),
        "patch": int(parts[2]),
    }


def pointer_depth(c_decl: str) -> int:
    return c_decl.count("*")


def is_const(c_decl: str) -> bool:
    return "const " in c_decl or c_decl.startswith("const")


def handle_kind(type_element: ET.Element) -> str:
    macro = text_of(type_element.find("type"))
    if macro == "VK_DEFINE_HANDLE":
        return "dispatchable"
    if macro == "VK_DEFINE_NON_DISPATCHABLE_HANDLE":
        return "non-dispatchable"
    return "unknown"


def collect_handles(root: ET.Element) -> dict[str, dict[str, str | None]]:
    handles: dict[str, dict[str, str | None]] = {}
    for type_element in root.findall("./types/type[@category='handle']"):
        name = text_of(type_element.find("name"))
        if not name:
            continue
        handles[name] = {
            "kind": handle_kind(type_element),
            "parent": type_element.get("parent"),
            "object_type": type_element.get("objtypeenum"),
        }
    return handles


def collect_structs(root: ET.Element, wanted: set[str]) -> dict[str, dict[str, object]]:
    structs: dict[str, dict[str, object]] = {}
    for type_element in root.findall("./types/type[@category='struct']"):
        name = type_element.get("name")
        if name not in wanted:
            continue
        members = []
        for member in type_element.findall("member"):
            member_name = text_of(member.find("name"))
            member_type = text_of(member.find("type"))
            declaration = declaration_of(member)
            members.append(
                {
                    "name": member_name,
                    "type": member_type,
                    "declaration": declaration,
                    "optional": member.get("optional") == "true",
                    "len": member.get("len"),
                    "values": member.get("values"),
                    "pointer_depth": pointer_depth(declaration),
                    "const": is_const(declaration),
                    "is_pnext": member_name == "pNext",
                    "is_string_array": member.get("len", "").endswith(
                        "null-terminated"
                    ),
                }
            )
        structs[name] = {
            "category": "struct",
            "members": members,
            "has_pnext": any(member["is_pnext"] for member in members),
        }
    return structs


def infer_command_level(params: list[dict[str, object]]) -> str:
    if params and params[0]["type"] == "VkInstance":
        return "instance"
    if params and params[0]["type"] == "VkPhysicalDevice":
        return "instance"
    if params and params[0]["type"] == "VkDevice":
        return "device"
    return "global"


def parameter_direction(param: dict[str, object]) -> str:
    name = str(param["name"])
    if name in {"pInstance", "pDevice"}:
        return "output"
    if param["pointer_depth"] and not param["const"]:
        return "output"
    return "input"


def classify_parameter(param_element: ET.Element) -> dict[str, object]:
    name = text_of(param_element.find("name"))
    param_type = text_of(param_element.find("type"))
    declaration = declaration_of(param_element)
    param: dict[str, object] = {
        "name": name,
        "type": param_type,
        "declaration": declaration,
        "optional": param_element.get("optional") == "true",
        "len": param_element.get("len"),
        "externsync": param_element.get("externsync"),
        "pointer_depth": pointer_depth(declaration),
        "const": is_const(declaration),
    }
    param["direction"] = parameter_direction(param)
    return param


def select_command(root: ET.Element, name: str) -> ET.Element:
    matches = [
        command
        for command in root.findall("./commands/command")
        if text_of(command.find("./proto/name")) == name
    ]
    if not matches:
        raise ValueError(f"command not found in Vulkan XML: {name}")
    vulkan_matches = [
        command
        for command in matches
        if command.get("api") in (None, "vulkan", "vulkan,vulkanbase")
    ]
    if not vulkan_matches:
        raise ValueError(f"no Vulkan command variant found: {name}")
    # Vulkan SC can define same-named commands with subtly different contracts.
    # The capture protocol must bind to one API dialect, so this first slice
    # deliberately keeps the standard Vulkan variant and records the source API.
    return vulkan_matches[0]


def command_metadata(
    root: ET.Element, name: str, handles: dict[str, dict[str, str | None]]
) -> dict[str, object]:
    command = select_command(root, name)
    params = [classify_parameter(param) for param in command.findall("param")]
    for param in params:
        handle = handles.get(str(param["type"]))
        param["handle_kind"] = handle["kind"] if handle else None
        param["handle_parent"] = handle["parent"] if handle else None
    handle_outputs = [
        param["type"]
        for param in params
        if param["direction"] == "output" and param["type"] in handles
    ]
    return {
        "id": stable_command_id(name),
        "revision": COMMAND_REVISION,
        "name": name,
        "api": command.get("api") or command.get("export"),
        "return_type": text_of(command.find("./proto/type")),
        "level": infer_command_level(params),
        "coverage_state": "unclassified",
        "successcodes": split_csv(command.get("successcodes")),
        "errorcodes": split_csv(command.get("errorcodes")),
        "dispatch_parameter": (
            params[0]["name"] if infer_command_level(params) != "global" else None
        ),
        "creates_handles": handle_outputs,
        "parameters": params,
    }


def stable_command_id(name: str) -> int:
    # Command IDs are part of the generated stream schema, so they must not
    # depend on registry ordering. The salt fixes this scheme for compatible schema
    # revisions while still deriving IDs mechanically from the API name.
    #
    # The raw hash is reduced modulo RESERVED_COMMAND_ID_BASE so the upper region
    # is structurally unreachable; manual command ids occupy that region. Zero is
    # reserved as a sentinel so the chunk header default of command_id=0 remains
    # an obvious "not yet set" rather than a valid generated command.
    digest = hashlib.sha256((COMMAND_ID_SALT + name).encode("utf-8")).digest()
    raw = int.from_bytes(digest[:4], byteorder="big")
    value = raw % RESERVED_COMMAND_ID_BASE
    return value or 1


def check_command_id_collisions(commands: list[dict[str, object]]) -> None:
    seen: dict[int, str] = {}
    for command in commands:
        command_id = int(command["id"])
        existing = seen.get(command_id)
        if existing is not None:
            raise ValueError(
                "stable command ID collision: "
                f"{existing} and {command['name']} both use {command_id}"
            )
        seen[command_id] = str(command["name"])


def write_coverage(metadata: dict[str, object], path: Path) -> None:
    commands = metadata["commands"]
    lines = [
        "# Vulkan Generated Coverage Slice",
        "",
        "<!-- Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand. -->",
        "",
        f"- Vulkan API version: {metadata['versions']['vulkan_api_version']}",
        f"- Header version: {metadata['versions']['header_version']}",
        f"- Generator version: {metadata['generator']['version']}",
        f"- Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}",
        "",
        "| Command | ID | Level | Coverage | Creates |",
        "| --- | ---: | --- | --- | --- |",
    ]
    for command in commands:
        creates = ", ".join(command["creates_handles"]) or ""
        lines.append(
            f"| `{command['name']}` | {command['id']} | {command['level']} | "
            f"{command['coverage_state']} | {creates} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def cxx_string_list(values: list[str]) -> str:
    if not values:
        return "{{}}"
    return "{{ " + ", ".join(f'"{value}"' for value in values) + " }}"


def command_enum_name(name: str) -> str:
    if not name.startswith("vk"):
        return name
    return name[2:]


def command_field_name(name: str) -> str:
    enum_name = command_enum_name(name)
    result = []
    for index, character in enumerate(enum_name):
        if character.isupper() and index != 0:
            result.append("_")
        result.append(character.lower())
    return "".join(result)


def parameter_cxx_type(parameter: dict[str, object]) -> str:
    declaration = str(parameter["declaration"])
    name = str(parameter["name"])
    suffix = f" {name}"
    if declaration.endswith(suffix):
        return declaration[: -len(suffix)]
    return declaration.removesuffix(name).rstrip()


def command_namespace(name: str) -> str:
    return name


def command_pfn_type(name: str) -> str:
    return f"PFN_{name}"


def forwarder_entrypoint_name(name: str) -> str:
    return f"{name}_entry"


def command_parameter_declarations(command: dict[str, object]) -> str:
    return ",\n    ".join(
        str(parameter["declaration"]) for parameter in command["parameters"]
    )


def command_parameter_names(command: dict[str, object]) -> str:
    return ", ".join(str(parameter["name"]) for parameter in command["parameters"])


def parameter_initializer_list(command: dict[str, object]) -> str:
    if not command["parameters"]:
        return "{}"
    fields = ", ".join(
        f".{parameter['name']} = {parameter['name']}"
        for parameter in command["parameters"]
    )
    return f"{{{fields}}}"


def output_parameter_assignments(command: dict[str, object]) -> str:
    lines = []
    parameters_by_name = {
        str(parameter["name"]): parameter for parameter in command["parameters"]
    }
    for parameter in command["parameters"]:
        if parameter["direction"] != "output":
            continue
        name = parameter["name"]
        length = parameter.get("len")
        count_name = str(length).split(",", 1)[0] if length else ""
        if count_name and count_name != "None":
            count_parameter = parameters_by_name.get(count_name, {})
            if int(count_parameter.get("pointer_depth", 0)) > 0:
                count_condition = f"response.{count_name}"
                count_value = f"*response.{count_name}"
            else:
                count_condition = f"response.{count_name}"
                count_value = f"response.{count_name}"
            lines.extend(
                [
                    f"  if ({name} && response.{name} &&",
                    f"      response.{name} != {name} &&",
                    f"      {count_condition}) {{",
                    f"    std::copy_n(response.{name},",
                    f"                {count_value}, {name});",
                    "  }",
                ]
            )
        else:
            lines.extend(
                [
                    f"  if ({name} && response.{name} &&",
                    f"      response.{name} != {name}) {{",
                    f"    *{name} = *response.{name};",
                    "  }",
                ]
            )
    return "\n".join(lines)


def default_return_statement(command: dict[str, object]) -> str:
    return_type = str(command["return_type"])
    if return_type == "void":
        return "  return;"
    if return_type == "VkResult":
        return "  return VK_SUCCESS;"
    return f"  return {return_type}{{}};"


def default_return_expression(command: dict[str, object]) -> str:
    return_type = str(command["return_type"])
    if return_type == "VkResult":
        return "VK_SUCCESS"
    return f"{return_type}{{}}"


def response_return_member(command: dict[str, object]) -> str:
    return_type = str(command["return_type"])
    if return_type == "void":
        return ""
    return f"  {return_type} return_value = {default_return_expression(command)};\n"


def response_output_fields(command: dict[str, object]) -> str:
    fields = []
    for parameter in command["parameters"]:
        if parameter["direction"] == "output":
            fields.append(
                f"  {parameter_cxx_type(parameter)} {parameter['name']} = {{}};"
            )
    output_array_counts = {
        str(parameter.get("len")).split(",", 1)[0]
        for parameter in command["parameters"]
        if parameter_is_output_array(parameter) and parameter.get("len")
    }
    output_names = {
        str(parameter["name"])
        for parameter in command["parameters"]
        if parameter["direction"] == "output"
    }
    for parameter in command["parameters"]:
        name = str(parameter["name"])
        if name in output_array_counts and name not in output_names:
            # Response payloads for create-style arrays need the caller's input
            # count so the forwarder can copy exactly the returned handles back.
            fields.append(f"  {parameter_cxx_type(parameter)} {name} = {{}};")
    return "\n".join(fields)


def command_needs_response(command: dict[str, object]) -> bool:
    if str(command["return_type"]) != "void":
        return True
    return any(
        parameter["direction"] == "output" for parameter in command["parameters"]
    )


def command_flushes_without_response(command: dict[str, object]) -> bool:
    return str(command["name"]) == "vkDestroyInstance"


def storage_struct_content(command: dict[str, object]) -> str:
    return """
struct ParameterStorage {};
"""


def response_initializer(command: dict[str, object]) -> str:
    fields = []
    if str(command["return_type"]) != "void":
        fields.append(f".return_value = {default_return_expression(command)}")
    for parameter in command["parameters"]:
        if parameter["direction"] == "output":
            fields.append(f".{parameter['name']} = {parameter['name']}")
    output_array_counts = {
        str(parameter.get("len")).split(",", 1)[0]
        for parameter in command["parameters"]
        if parameter_is_output_array(parameter) and parameter.get("len")
    }
    output_names = {
        str(parameter["name"])
        for parameter in command["parameters"]
        if parameter["direction"] == "output"
    }
    for parameter in command["parameters"]:
        name = str(parameter["name"])
        if name in output_array_counts and name not in output_names:
            fields.append(f".{name} = {name}")
    if not fields:
        return "{}"
    return "{" + ", ".join(fields) + "}"


def response_return_statement(command: dict[str, object]) -> str:
    if str(command["return_type"]) == "void":
        return "  return;"
    return "  return response.return_value;"


def status_failure_return_statement(
    command: dict[str, object], status_name: str
) -> str:
    if str(command["return_type"]) == "void":
        return "    return;"
    return f"    return {status_name};"


def write_manual_hooks_header(metadata: dict[str, object], path: Path) -> None:
    content = f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

namespace vkfwd::manual {{

template <vkfwd::generated::CommandId>
struct CommandHooks {{
  static constexpr bool before_pack_enabled = false;
  static constexpr bool after_pack_enabled = false;
  static constexpr bool before_unpack_enabled = false;
  static constexpr bool after_unpack_enabled = false;

  template <class Parameters>
  static constexpr void before_pack(Parameters&) noexcept {{}}

  static constexpr void after_pack() noexcept {{}}

  template <class View>
  static constexpr void before_unpack(View&) noexcept {{}}

  template <class Parameters>
  static constexpr void after_unpack(Parameters&) noexcept {{}}
}};

}} // namespace vkfwd::manual
"""
    path.write_text(content, encoding="utf-8")


def command_header_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    fields = "\n".join(
        f"  {parameter_cxx_type(parameter)} {parameter['name']} = {{}};"
        for parameter in command["parameters"]
    )
    response_methods = ""
    response_struct = ""
    if command_needs_response(command):
        response_struct = f"""
struct Response {{
{response_return_member(command)}{response_output_fields(command)}
}};
"""
        response_methods = f"""
  using Response = vkfwd::generated::commands::{namespace}::Response;

  static VkResult pack_response(CommandStream& stream,
                                const Response& response);
  static VkResult unpack_response(SafeArrayView<std::uint8_t>& view,
                                  const Response** response);
"""
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_id_range.hpp"
#include "generated/vulkan_api.hpp"
#include "generated/vulkan_manual_hooks.hpp"
#include "command_stream.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

// Generated command ids must never collide with the reserved manual range used
// by vkfwd-owned wire commands. The generator enforces this by constraining its
// hash output below kReservedCommandIdBase; this per-command static_assert is
// the structural check that the invariant survives any future edit.
static_assert(static_cast<std::uint32_t>(::vkfwd::generated::CommandId::{enum_name}) < ::vkfwd::kReservedCommandIdBase,
              "generated command id must not occupy the reserved manual range");

namespace vkfwd::generated::commands::{namespace} {{

struct Parameters {{
{fields}
}};
{response_struct}

class Command {{
public:
  using Parameters = vkfwd::generated::commands::{namespace}::Parameters;

  static VkResult pack_parameters(CommandStream& stream,
                                  const Parameters& parameters);
  static VkResult unpack_parameters(SafeArrayView<std::uint8_t>& view,
                                    const Parameters** parameters);
{response_methods}
}};

}} // namespace vkfwd::generated::commands::{namespace}

#if __has_include("hook/{command['name']}Hook.hpp")
#include "hook/{command['name']}Hook.hpp"
#endif
"""


def command_source_helpers(command: dict[str, object]) -> str:
    enum_name = command_enum_name(str(command["name"]))
    return f"""
template<class T>
constexpr std::size_t command_payload_offset() {{
  // Command chunks always begin with the fixed protocol header:
  // command id, total chunk size including this header, and command revision.
  // Payload alignment padding follows that header and is included in size.
  constexpr std::size_t kPayloadAlignment = alignof(T);
  return (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
}}

template<class Pointer>
VkResult patch_command_pointer(Pointer& pointer_slot, std::size_t pointer_slot_offset, std::size_t target_offset) {{
  // Command-level pointers use the same field-relative offset rule as
  // structures: the encoded value is measured from the pointer slot itself.
  // Unpack can therefore repair pointers in-place without remembering whether a
  // slot came from command parameters or from a nested Vulkan struct.
  pointer_slot = reinterpret_cast<Pointer>(target_offset ? static_cast<std::uintptr_t>(target_offset - pointer_slot_offset) : 0);
  return VK_SUCCESS;
}}

template<class Pointer>
VkResult recover_command_pointer(Pointer& pointer_slot, SafeArrayView<std::uint8_t>& view) {{
  if (!pointer_slot) {{ return VK_SUCCESS; }}
  auto* begin = view.address(0);
  if (!begin) [[unlikely]] {{ return VK_ERROR_UNKNOWN; }}
  auto* slot = reinterpret_cast<std::uint8_t*>(&pointer_slot);
  auto* end = begin + view.size();
  auto* target = slot + reinterpret_cast<std::uintptr_t>(pointer_slot);
  if (slot < begin || slot + sizeof(Pointer) > end || target < begin || target >= end) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command unpack failed: encoded pointer is outside command view, slot={{}}, target={{}}, view_size={{}}",
                    static_cast<const void*>(slot), static_cast<const void*>(target), view.size());
    return VK_ERROR_UNKNOWN;
  }}
  pointer_slot = reinterpret_cast<Pointer>(target);
  return VK_SUCCESS;
}}

SafeArrayView<std::uint8_t> tail_view_from_pointer(SafeArrayView<std::uint8_t>& view, const void* pointer) {{
  auto* begin = view.address(0);
  if (!begin || !pointer) {{ return {{}}; }}
  auto* target = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(pointer));
  auto* end = begin + view.size();
  if (target < begin || target >= end) {{ return {{}}; }}
  return SafeArrayView<std::uint8_t>(static_cast<std::size_t>(end - target), target);
}}

VkResult pack_allocator(const VkAllocationCallbacks* allocator,
                        CommandStream& stream,
                        std::size_t pointer_slot_offset,
                        const VkAllocationCallbacks*& pointer_slot) {{
  (void)allocator;
  (void)stream;
  // Vulkan allocation callbacks are guest-process function pointers and user
  // data. They have no valid receiver-process address, so the wire contract is
  // to drop them and replay with the receiver's default allocator.
  return patch_command_pointer(pointer_slot, pointer_slot_offset, 0);
}}

template<class Pointer>
VkResult pack_output_pointer(Pointer value, CommandStream& stream, std::size_t pointer_slot_offset, Pointer& pointer_slot) {{
  using Pointee = std::remove_pointer_t<Pointer>;
  if (!value) [[unlikely]] {{ return patch_command_pointer(pointer_slot, pointer_slot_offset, 0); }}
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<Pointee>(1, alignof(Pointee), &target);
    if (!destination.set(0, *value)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: could not copy output value into stream, size={{}}", sizeof(Pointee));
      return VK_ERROR_UNKNOWN;
    }}
    return patch_command_pointer(pointer_slot, pointer_slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: out of host memory while copying output value, size={{}}", sizeof(Pointee));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

VkResult pack_input_string(const char* value, CommandStream& stream, std::size_t pointer_slot_offset, const char*& pointer_slot) {{
  if (!value) [[unlikely]] {{ return patch_command_pointer(pointer_slot, pointer_slot_offset, 0); }}
  try {{
    const std::size_t size = std::strlen(value) + 1;
    std::size_t target = 0;
    auto destination = stream.grow<char>(size, alignof(char), &target);
    if (destination.set(0, size, value) != size) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy input string, pointer_slot_offset={{}}", pointer_slot_offset);
      return VK_ERROR_UNKNOWN;
    }}
    return patch_command_pointer(pointer_slot, pointer_slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while copying input string, pointer_slot_offset={{}}", pointer_slot_offset);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

template<class Pointer>
VkResult pack_output_array(Pointer value, std::uint32_t count, CommandStream& stream, std::size_t pointer_slot_offset, Pointer& pointer_slot) {{
  using Pointee = std::remove_pointer_t<Pointer>;
  if (!value || count == 0) [[unlikely]] {{ return patch_command_pointer(pointer_slot, pointer_slot_offset, 0); }}
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<Pointee>(count, alignof(Pointee), &target);
    if (destination.set(0, count, value) != count) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: could not copy output array, count={{}}, element_size={{}}", count, sizeof(Pointee));
      return VK_ERROR_UNKNOWN;
    }}
    return patch_command_pointer(pointer_slot, pointer_slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: out of host memory while copying output array, count={{}}, element_size={{}}", count, sizeof(Pointee));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

template<class T>
VkResult append_command_chunk(CommandStream& stream, CommandId command_id, std::uint32_t revision, const T& payload, Range& range, T*& packed_payload) {{
  constexpr std::size_t kPayloadOffset = command_payload_offset<T>();
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);

  // The chunk is one contiguous serialized range. Its fixed header is command
  // id, chunk size including the header, and command revision; payload starts
  // at an aligned offset after those fields.
  if constexpr (kCommandSize > std::numeric_limits<std::uint32_t>::max()) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: command chunk is too large, command_id={{}}, command_size={{}}",
                    static_cast<std::uint32_t>(command_id), kCommandSize);
    return VK_ERROR_UNKNOWN;
  }}

  range = Range{{.offset = 0, .size = 0}};
  packed_payload = nullptr;

  CommandChunkHeader header{{}};
  try {{
    std::size_t offset = 0;
    auto destination = stream.grow<std::uint8_t>(kCommandSize, CommandStream::kBaseAlignment, &offset);
    header.command_id = static_cast<std::uint32_t>(command_id);
    header.size = static_cast<std::uint32_t>(kCommandSize);
    header.command_revision = revision;

    if (destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t*>(&header)) != sizeof(header) ||
        destination.set(kPayloadOffset, sizeof(payload), reinterpret_cast<const std::uint8_t*>(&payload)) != sizeof(payload)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy command chunk, command_id={{}}, command_size={{}}",
                      static_cast<std::uint32_t>(command_id), kCommandSize);
      return VK_ERROR_UNKNOWN;
    }}
    range.offset = offset;
    range.size = header.size;
    packed_payload = reinterpret_cast<T*>(&destination.at(kPayloadOffset));
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={{}}, payload_size={{}}",
                    static_cast<std::uint32_t>(command_id), sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
  return VK_SUCCESS;
}}

template<class T>
VkResult append_command_chunk(CommandStream& stream, CommandId command_id, std::uint32_t revision, const T& payload, Range& range) {{
  T* packed_payload = nullptr;
  return append_command_chunk(stream, command_id, revision, payload, range, packed_payload);
}}

VkResult finalize_command_chunk(CommandStream& stream, Range& range) {{
  const std::size_t command_size = stream.size() - range.offset;
  if (command_size > std::numeric_limits<std::uint32_t>::max()) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: finalized command chunk is too large, offset={{}}, size={{}}",
                    range.offset, command_size);
    return VK_ERROR_UNKNOWN;
  }}

  auto header_view = stream.at<CommandChunkHeader>(range.offset);
  auto* header = header_view.address();
  if (!header) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not rewrite command chunk size, offset={{}}", range.offset);
    return VK_ERROR_UNKNOWN;
  }}

  header->size = static_cast<std::uint32_t>(command_size);
  range.size = header->size;
  return VK_SUCCESS;
}}

template<class T>
VkResult unpack_command_chunk(SafeArrayView<std::uint8_t>& view, CommandId command_id, std::uint32_t revision, T** payload) {{
  constexpr std::size_t kPayloadOffset = command_payload_offset<T>();
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);
  auto* header = view.size() < sizeof(CommandChunkHeader) ? nullptr : reinterpret_cast<CommandChunkHeader*>(view.address(0));
  auto* packed_payload = !header || view.size() < kCommandSize
      ? nullptr
      : reinterpret_cast<T*>(view.address(kPayloadOffset));
  if (!header || !packed_payload || header->command_id != static_cast<std::uint32_t>(command_id) || header->command_revision != revision ||
      header->size < kCommandSize || view.size() < header->size) [[unlikely]] {{
    VKFWD_LOG_ERROR(
        "vkfwd ferry command unpack failed: invalid command view, view_size={{}}, has_header={{}}, has_payload={{}}, command_id={{}}, "
        "expected_command_id={{}}, revision={{}}, expected_revision={{}}, header_size={{}}, expected_size={{}}",
        view.size(), header != nullptr, packed_payload != nullptr, header ? header->command_id : 0,
        static_cast<std::uint32_t>(command_id), header ? header->command_revision : 0, revision, header ? header->size : 0, kCommandSize);
    return VK_ERROR_UNKNOWN;
  }}

  *payload = packed_payload;
  return VK_SUCCESS;
}}
"""


def parameter_is_copied_input_pointer(parameter: dict[str, object]) -> bool:
    if parameter["direction"] == "output" and int(parameter["pointer_depth"]) == 1:
        return True
    return int(parameter["pointer_depth"]) == 1 and (
        str(parameter["type"]) in {"char", "VkAllocationCallbacks"}
        or str(parameter["type"]) in SUPPORTED_COMMAND_STRUCT_PARAMETERS
    )


def parameter_is_output_array(parameter: dict[str, object]) -> bool:
    return (
        parameter["direction"] == "output"
        and int(parameter["pointer_depth"]) == 1
        and bool(parameter.get("len"))
    )


def output_array_count_expression(
    command: dict[str, object], parameter: dict[str, object], source_name: str
) -> str:
    length_name = str(parameter.get("len") or "")
    parameters_by_name = {
        str(candidate["name"]): candidate for candidate in command["parameters"]
    }
    count_parameter = parameters_by_name.get(length_name)
    if not count_parameter:
        return "0u"
    if int(count_parameter["pointer_depth"]) > 0:
        return f"{source_name}.{length_name} ? *{source_name}.{length_name} : 0u"
    return f"{source_name}.{length_name}"


def command_pointer_pack_lines(
    command: dict[str, object], source_name: str
) -> list[str]:
    lines: list[str] = []
    for parameter in command["parameters"]:
        if not parameter_is_copied_input_pointer(parameter):
            continue
        name = str(parameter["name"])
        ptype = str(parameter["type"])
        slot = f"payload_offset + offsetof(Parameters, {name})"
        if ptype == "VkAllocationCallbacks":
            lines.extend(
                [
                    f"  status = pack_allocator({source_name}.{name}, stream, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        elif ptype == "char" and parameter["direction"] == "input":
            lines.extend(
                [
                    f"  status = pack_input_string({source_name}.{name}, stream, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        elif (
            ptype in SUPPORTED_COMMAND_STRUCT_PARAMETERS
            and parameter["direction"] == "input"
            and bool(parameter.get("len"))
        ):
            count_expression = output_array_count_expression(
                command, parameter, source_name
            )
            lines.extend(
                [
                    f"  structure::PackedStruct packed_{name};",
                    f"  status = structure::pack_array_{ptype}({source_name}.{name}, {count_expression}, stream, packed_{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                    f"  status = patch_command_pointer(packed_parameters->{name}, {slot}, packed_{name}.offset);",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        elif parameter_is_output_array(parameter):
            count_expression = output_array_count_expression(
                command, parameter, source_name
            )
            lines.extend(
                [
                    f"  status = pack_output_array({source_name}.{name}, {count_expression}, stream, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        elif parameter["direction"] == "output":
            lines.extend(
                [
                    f"  status = pack_output_pointer({source_name}.{name}, stream, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        else:
            lines.extend(
                [
                    f"  structure::PackedStruct packed_{name};",
                    f"  status = structure::pack_{ptype}({source_name}.{name}, stream, packed_{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                    f"  status = patch_command_pointer(packed_parameters->{name}, {slot}, packed_{name}.offset);",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
    return lines


def command_parameter_recover_lines(command: dict[str, object]) -> list[str]:
    lines: list[str] = []
    for parameter in command["parameters"]:
        if not parameter_is_copied_input_pointer(parameter):
            continue
        name = str(parameter["name"])
        ptype = str(parameter["type"])
        lines.extend(
            [
                f"  status = recover_command_pointer(packed_parameters->{name}, view);",
                "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
            ]
        )
        if ptype in SUPPORTED_COMMAND_STRUCT_PARAMETERS:
            count_expression = output_array_count_expression(
                command, parameter, "(*packed_parameters)"
            )
            if bool(parameter.get("len")):
                lines.extend(
                    [
                        f"  if (packed_parameters->{name}) {{",
                        f"    auto child_view = tail_view_from_pointer(view, packed_parameters->{name});",
                        f"    const {ptype}* ignored_{name} = nullptr;",
                        f"    status = structure::unpack_array_{ptype}(child_view, {count_expression}, &ignored_{name});",
                        "    if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                        "  }",
                    ]
                )
                continue
            lines.extend(
                [
                    f"  if (packed_parameters->{name}) {{",
                    f"    auto child_view = tail_view_from_pointer(view, packed_parameters->{name});",
                    f"    const {ptype}* ignored_{name} = nullptr;",
                    f"    status = structure::unpack_{ptype}(child_view, &ignored_{name});",
                    "    if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                    "  }",
                ]
            )
    return lines


def response_pointer_pack_lines(command: dict[str, object]) -> list[str]:
    lines: list[str] = []
    for parameter in command["parameters"]:
        if parameter["direction"] != "output" or int(parameter["pointer_depth"]) != 1:
            continue
        name = str(parameter["name"])
        ptype = str(parameter["type"])
        slot = f"payload_offset + offsetof(Response, {name})"
        if parameter_is_output_array(parameter):
            count_expression = output_array_count_expression(
                command, parameter, "response"
            )
            lines.extend(
                [
                    f"  status = pack_output_array(response.{name}, {count_expression}, stream, {slot}, packed_response->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
            continue
        lines.extend(
            [
                f"  status = pack_output_pointer(response.{name}, stream, {slot}, packed_response->{name});",
                "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
            ]
        )
    return lines


def response_pointer_recover_lines(command: dict[str, object]) -> list[str]:
    lines: list[str] = []
    for parameter in command["parameters"]:
        if parameter["direction"] != "output" or int(parameter["pointer_depth"]) != 1:
            continue
        name = str(parameter["name"])
        lines.extend(
            [
                f"  status = recover_command_pointer(packed_response->{name}, view);",
                "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
            ]
        )
    return lines


def command_pack_body(
    command: dict[str, object], enum_name: str, source_name: str
) -> str:
    pointer_lines = "\n".join(command_pointer_pack_lines(command, source_name))
    if pointer_lines:
        pointer_lines = "\n" + pointer_lines
    return f"""
  Parameters* packed_parameters = nullptr;
  Range range;
  VkResult status = append_command_chunk(stream, CommandId::{enum_name}, {COMMAND_REVISION}, {source_name}, range, packed_parameters);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  const std::size_t payload_offset = range.offset + command_payload_offset<Parameters>();
{pointer_lines}
  status = finalize_command_chunk(stream, range);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
"""


def command_source_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    helpers = command_source_helpers(command)
    needs_structure = any(
        parameter_is_copied_input_pointer(parameter)
        and str(parameter["type"]) != "VkAllocationCallbacks"
        for parameter in command["parameters"]
    )
    command_struct_include = any(
        int(parameter["pointer_depth"]) == 1
        and str(parameter["type"]) in SUPPORTED_COMMAND_STRUCT_PARAMETERS
        for parameter in command["parameters"]
    )
    structure_include = (
        '#include "generated/structure/core.hpp"\n#include "generated/structure/command_structs.hpp"'
        if command_struct_include
        else ('#include "generated/structure/core.hpp"' if needs_structure else "")
    )
    helpers_block = (
        f"""namespace {{

{helpers}

}} // namespace
"""
        if helpers.strip()
        else ""
    )
    pack_body = command_pack_body(command, enum_name, "parameters")
    hook_pack_body = command_pack_body(command, enum_name, "hook_parameters")
    parameter_recover_lines = "\n".join(command_parameter_recover_lines(command))
    response_methods = ""
    if command_needs_response(command):
        response_pack_lines = "\n".join(response_pointer_pack_lines(command))
        if response_pack_lines:
            response_pack_lines = "\n" + response_pack_lines
        response_recover_lines = "\n".join(response_pointer_recover_lines(command))
        if response_recover_lines:
            response_recover_lines = "\n" + response_recover_lines
        response_methods = f"""
VkResult Command::pack_response(CommandStream& stream,
                                const Response& response) {{
  Response* packed_response = nullptr;
  Range range;
  VkResult status = append_command_chunk(stream, CommandId::{enum_name}, {COMMAND_REVISION}, response, range, packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  const std::size_t payload_offset = range.offset + command_payload_offset<Response>();
{response_pack_lines}
  status = finalize_command_chunk(stream, range);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return VK_SUCCESS;
}}

VkResult Command::unpack_response(SafeArrayView<std::uint8_t>& view,
                                  const Response** response) {{
  Response* packed_response = nullptr;
  VkResult status = unpack_command_chunk(view, CommandId::{enum_name}, {COMMAND_REVISION}, &packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
{response_recover_lines}
  *response = packed_response;
  return VK_SUCCESS;
}}
"""
    return f"""#include "generated/command/{command['name']}.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "logging.hpp"
{structure_include}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace vkfwd::generated::commands::{namespace} {{
{helpers_block}

VkResult Command::pack_parameters(CommandStream& stream,
                                  const Parameters& parameters) {{
  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_pack_enabled) {{
    Parameters hook_parameters = parameters;
    Hooks::before_pack(hook_parameters);

{hook_pack_body}

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack();
    }}
    return VK_SUCCESS;
  }} else {{
{pack_body}

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack();
    }}
    return VK_SUCCESS;
  }}
}}

VkResult Command::unpack_parameters(SafeArrayView<std::uint8_t>& view,
                                    const Parameters** parameters) {{
  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_unpack_enabled) {{
    Hooks::before_unpack(view);
  }}

  Parameters* packed_parameters = nullptr;
  VkResult status = unpack_command_chunk(view, CommandId::{enum_name}, {COMMAND_REVISION}, &packed_parameters);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
{parameter_recover_lines}
  *parameters = packed_parameters;

  if constexpr (Hooks::after_unpack_enabled) {{
    Hooks::after_unpack(*packed_parameters);
  }}
  return VK_SUCCESS;
}}
{response_methods}

}} // namespace vkfwd::generated::commands::{namespace}
"""


def write_command_files(metadata: dict[str, object], output_dir: Path) -> None:
    commands_dir = output_dir / "command"
    commands_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "README.md").write_text(
        """# Generated Vulkan Code

Every file in this directory tree is produced by
`src/vkfwd/ferry/script/generator/vulkan_metadata.py`. Do not place manual code here; regeneration
may replace these files without preserving local edits.

Per-command generated code and per-command generated metadata live under
`command/`. Human-written hook code belongs under
`src/vkfwd/ferry/core/hook/<api>Hook.hpp` and optional matching `.cpp` files.
`vulkan_api.hpp` contains shared generated API facts such as stable command ids
and the pinned Vulkan API version. `dispatch_table.hpp` and
`dispatch_table.cpp` contain shared generated Vulkan function-pointer table
types and name lookup helpers. There is intentionally no generated
`vulkan_api.cpp`; command metadata stays per-command, while table lookup stays
with the shared generated API surface.
""",
        encoding="utf-8",
    )
    for command in metadata["commands"]:
        (commands_dir / f"{command['name']}.hpp").write_text(
            command_header_content(metadata, command), encoding="utf-8"
        )
        (commands_dir / f"{command['name']}.cpp").write_text(
            command_source_content(metadata, command), encoding="utf-8"
        )
        (commands_dir / f"{command['name']}.metadata.json").write_text(
            json.dumps(
                command_metadata_document(metadata, command), indent=2, sort_keys=True
            )
            + "\n",
            encoding="utf-8",
        )


def command_metadata_document(
    metadata: dict[str, object], command: dict[str, object]
) -> dict[str, object]:
    handles = metadata["handles"]
    structs = metadata["structs"]
    handle_names = sorted(
        {
            str(parameter["type"])
            for parameter in command["parameters"]
            if str(parameter["type"]) in handles
        }
        | {str(handle) for handle in command["creates_handles"]}
    )
    struct_names = sorted(
        {
            str(parameter["type"])
            for parameter in command["parameters"]
            if str(parameter["type"]) in structs
        }
    )
    return {
        "schema": "vkfwd.vulkan-command-metadata.v1",
        "generator": metadata["generator"],
        "protocol": metadata["protocol"],
        "versions": metadata["versions"],
        "command": command,
        "handles": {name: handles[name] for name in handle_names},
        "structs": {name: structs[name] for name in struct_names},
    }


def write_manifest(metadata: dict[str, object], path: Path) -> None:
    manifest = {
        "schema": "vkfwd.vulkan-generation-manifest.v1",
        "generator": metadata["generator"],
        "protocol": metadata["protocol"],
        "versions": metadata["versions"],
        "command_count": len(metadata["commands"]),
        "commands": [command["name"] for command in metadata["commands"]],
        "command_metadata": [
            f"command/{command['name']}.metadata.json"
            for command in metadata["commands"]
        ],
    }
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_vulkan_api_header(metadata: dict[str, object], path: Path) -> None:
    commands = metadata["commands"]
    enum_values = "\n".join(
        f"  {command_enum_name(command['name'])} = {command['id']},"
        for command in commands
    )
    content = f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <cstdint>

namespace vkfwd {{

struct VulkanApiVersion {{
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
}};

namespace generated {{

// CommandId values are part of the schema-versioned command envelope. They are
// generated from stable command names instead of registry order so compatible
// Vulkan XML revisions can add or remove commands without renumbering the stream.
enum class CommandId : std::uint32_t {{
{enum_values}
}};

constexpr VulkanApiVersion kVulkanApiVersion{{
    {metadata['versions']['vulkan_api']['major']},
    {metadata['versions']['vulkan_api']['minor']},
    {metadata['versions']['vulkan_api']['patch']}}};

}} // namespace generated
}} // namespace vkfwd
"""
    path.write_text(content, encoding="utf-8")


def forwarder_hooks_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

namespace vkfwd::forwarder::manual {{

template <vkfwd::generated::CommandId>
struct CommandHooks {{
  static constexpr bool before_pack_enabled = false;
  static constexpr bool after_pack_enabled = false;
  static constexpr bool after_response_unpack_enabled = false;

  template <class... Args>
  static constexpr void before_pack(Args&...) noexcept {{}}

  template <class Parameters>
  static constexpr void after_pack(const Parameters&) noexcept {{}}

  template <class Parameters, class Response>
  static constexpr void after_response_unpack(const Parameters&, Response&) noexcept {{}}
}};

}} // namespace vkfwd::forwarder::manual
"""


def receiver_hooks_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

namespace vkfwd::receiver::manual {{

// Per-API receiver-endpoint customization. Hook authors specialize this template
// in receiver/hook/<api>ReceiverHook.hpp. The generated endpoint stub conditionally
// calls each phase via `if constexpr (Hooks::<phase>_enabled)`. When
// replace_endpoint_enabled is true the stub bypasses the standard
// unpack/call/pack body entirely and forwards the whole call to replace_endpoint;
// the other phase flags are then ignored.
template <vkfwd::generated::CommandId>
struct CommandHooks {{
  static constexpr bool before_unpack_enabled = false;
  static constexpr bool before_call_enabled = false;
  static constexpr bool after_call_enabled = false;
  static constexpr bool before_pack_response_enabled = false;
  static constexpr bool after_pack_response_enabled = false;
  static constexpr bool replace_endpoint_enabled = false;

  template <class... Args>
  static constexpr void before_unpack(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void before_call(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void after_call(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void before_pack_response(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void after_pack_response(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr bool replace_endpoint(Args&...) noexcept {{ return false; }}
}};

}} // namespace vkfwd::receiver::manual
"""


def dispatch_table_header_content(metadata: dict[str, object]) -> str:
    commands = list(metadata["commands"])
    global_members = "\n".join(
        f"  {command_pfn_type(str(command['name']))} {command_field_name(str(command['name']))} = nullptr;"
        for command in commands
        if dispatch_table_group(command) == "global"
    )
    instance_members = "\n".join(
        f"  {command_pfn_type(str(command['name']))} {command_field_name(str(command['name']))} = nullptr;"
        for command in commands
        if dispatch_table_group(command) == "instance"
    )
    device_members = "\n".join(
        f"  {command_pfn_type(str(command['name']))} {command_field_name(str(command['name']))} = nullptr;"
        for command in commands
        if dispatch_table_group(command) == "device"
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

#include <cstdint>
#include <unordered_map>

#include <vulkan/vulkan.h>

namespace vkfwd::generated {{

using PointerToFunctionPointer = PFN_vkVoidFunction*;

struct GlobalDispatchTable {{
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
{global_members}

  void init(PFN_vkGetInstanceProcAddr get_instance_proc_addr);
  PFN_vkVoidFunction getProcByName(const char* name) const;
}};

struct InstanceDispatchTable {{
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
{instance_members}

  void init(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr);
  PFN_vkVoidFunction getProcByName(const char* name) const;
}};

struct DeviceDispatchTable {{
{device_members}

  void init(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr);
  PFN_vkVoidFunction getProcByName(const char* name) const;
}};

struct DistributionTable {{
  GlobalDispatchTable global {{}};
  InstanceDispatchTable instance {{}};
  DeviceDispatchTable device {{}};

  const std::unordered_map<std::uint32_t, PointerToFunctionPointer> commands;

  DistributionTable();
  DistributionTable(const DistributionTable&) = delete;
  DistributionTable& operator=(const DistributionTable&) = delete;

  PFN_vkVoidFunction getProcByName(const char* name) const;
  PFN_vkVoidFunction getProcByCommandId(CommandId command_id) const;
}};

}} // namespace vkfwd::generated
"""


def dispatch_table_group(command: dict[str, object]) -> str:
    name = str(command["name"])
    parameters = list(command["parameters"])
    if command["level"] == "global":
        return "global"
    if parameters and parameters[0]["type"] == "VkDevice":
        return "device"
    return "instance"


def dispatch_table_member_access(command: dict[str, object]) -> str:
    level = dispatch_table_group(command)
    field = command_field_name(str(command["name"]))
    if level == "global":
        return f"global.{field}"
    if level == "instance":
        return f"instance.{field}"
    if level == "device":
        return f"device.{field}"
    raise ValueError(f"unsupported command level for dispatch table: {level}")


def command_pointer_map_entries(commands: list[dict[str, object]]) -> str:
    return "\n".join(
        "    {"
        f"static_cast<std::uint32_t>(CommandId::{command_enum_name(str(command['name']))}), "
        f"reinterpret_cast<PointerToFunctionPointer>(&{dispatch_table_member_access(command)})"
        "},"
        for command in commands
    )


def dispatch_table_init_lines(commands: list[dict[str, object]], group: str) -> str:
    lines = []
    for command in commands:
        if dispatch_table_group(command) != group:
            continue
        name = str(command["name"])
        field = command_field_name(name)
        pfn = command_pfn_type(name)
        if group == "global":
            lines.extend(
                [
                    f"  {field} = get_instance_proc_addr",
                    f'      ? typed_proc<{pfn}>(get_instance_proc_addr(nullptr, "{name}"))',
                    "      : nullptr;",
                ]
            )
        elif group == "instance":
            lines.append(
                f'  {field} = typed_proc<{pfn}>(get_instance_proc_addr(instance, "{name}"));'
            )
        elif group == "device":
            lines.append(
                f'  {field} = typed_proc<{pfn}>(get_device_proc_addr(device, "{name}"));'
            )
        else:
            raise ValueError(f"unsupported dispatch-table group: {group}")
    return "\n".join(lines)


def dispatch_table_null_lines(commands: list[dict[str, object]], group: str) -> str:
    lines = []
    if group == "instance":
        lines.append("    get_device_proc_addr = nullptr;")
    for command in commands:
        if dispatch_table_group(command) == group:
            lines.append(f"    {command_field_name(str(command['name']))} = nullptr;")
    return "\n".join(lines)


def dispatch_table_lookup_lines(commands: list[dict[str, object]], group: str) -> str:
    lines = []
    for command in commands:
        if dispatch_table_group(command) != group:
            continue
        name = str(command["name"])
        field = command_field_name(name)
        lines.extend(
            [
                f'  if (std::strcmp(name, "{name}") == 0) {{',
                f"    return reinterpret_cast<PFN_vkVoidFunction>({field});",
                "  }",
            ]
        )
    return "\n".join(lines)


def dispatch_table_source_content(metadata: dict[str, object]) -> str:
    commands = list(metadata["commands"])
    pointer_map_entries = command_pointer_map_entries(commands)
    global_init_lines = dispatch_table_init_lines(commands, "global")
    instance_init_lines = dispatch_table_init_lines(commands, "instance")
    device_init_lines = dispatch_table_init_lines(commands, "device")
    instance_null_lines = dispatch_table_null_lines(commands, "instance")
    device_null_lines = dispatch_table_null_lines(commands, "device")
    global_lookup_lines = dispatch_table_lookup_lines(commands, "global")
    instance_lookup_lines = dispatch_table_lookup_lines(commands, "instance")
    device_lookup_lines = dispatch_table_lookup_lines(commands, "device")
    return f"""#include "generated/dispatch_table.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <cstring>

namespace vkfwd::generated {{
namespace {{

template<class FunctionPointer>
FunctionPointer typed_proc(PFN_vkVoidFunction proc) {{
  return reinterpret_cast<FunctionPointer>(proc);
}}

}} // namespace

DistributionTable::DistributionTable()
    : commands {{
{pointer_map_entries}
      }} {{}}

void GlobalDispatchTable::init(PFN_vkGetInstanceProcAddr get_instance_proc_addr_) {{
  get_instance_proc_addr = get_instance_proc_addr_;

  // Global commands are loaded before any VkInstance exists, so the Vulkan
  // loader contract requires a null instance handle for these lookups.
{global_init_lines}
}}

void InstanceDispatchTable::init(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr) {{
  // The instance table is populated only after vkCreateInstance succeeds. The
  // instance handle scopes extension/core lookup and must remain valid while
  // these dispatch slots are used.
  if (!get_instance_proc_addr) {{
{instance_null_lines}
    return;
  }}

  get_device_proc_addr = typed_proc<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr(instance, "vkGetDeviceProcAddr"));
{instance_init_lines}
}}

void DeviceDispatchTable::init(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr) {{
  // Device commands are loaded after vkCreateDevice succeeds. The device
  // dispatch slots are scoped to that destination device and should be
  // refreshed for each replay/device mapping.
  if (!get_device_proc_addr) {{
{device_null_lines}
    return;
  }}

{device_init_lines}
}}

PFN_vkVoidFunction GlobalDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(get_instance_proc_addr);
  }}
{global_lookup_lines}
  return nullptr;
}}

PFN_vkVoidFunction InstanceDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(get_device_proc_addr);
  }}
{instance_lookup_lines}
  return nullptr;
}}

PFN_vkVoidFunction DeviceDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
{device_lookup_lines}
  return nullptr;
}}

PFN_vkVoidFunction DistributionTable::getProcByName(const char* name) const {{
  if (auto entrypoint = global.getProcByName(name)) {{ return entrypoint; }}
  if (auto entrypoint = instance.getProcByName(name)) {{ return entrypoint; }}
  return device.getProcByName(name);
}}

PFN_vkVoidFunction DistributionTable::getProcByCommandId(CommandId command_id) const {{
  const auto entry = commands.find(static_cast<std::uint32_t>(command_id));
  if (entry == commands.end() || !entry->second) {{ return nullptr; }}
  return *entry->second;
}}

}} // namespace vkfwd::generated
"""


def forwarder_entrypoints_header_content(metadata: dict[str, object]) -> str:
    declarations = "\n".join(
        f"VKAPI_ATTR {command['return_type']} VKAPI_CALL {forwarder_entrypoint_name(str(command['name']))}(\n"
        f"    {command_parameter_declarations(command)});"
        for command in metadata["commands"]
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"
#include "generated/dispatch_table.hpp"

#include <vulkan/vulkan.h>

namespace vkfwd::forwarder::generated {{

{declarations}

const ::vkfwd::generated::GlobalDispatchTable& global_dispatch_table();
const ::vkfwd::generated::InstanceDispatchTable& instance_dispatch_table();
const ::vkfwd::generated::DeviceDispatchTable& device_dispatch_table();

}} // namespace vkfwd::forwarder::generated
"""


def forwarder_entrypoints_source_content(metadata: dict[str, object]) -> str:
    commands = list(metadata["commands"])
    global_entries = "\n".join(
        f"  .{command_field_name(str(command['name']))} = {forwarder_entrypoint_name(str(command['name']))},"
        for command in commands
        if dispatch_table_group(command) == "global"
    )
    instance_entries = "\n".join(
        f"  .{command_field_name(str(command['name']))} = {forwarder_entrypoint_name(str(command['name']))},"
        for command in commands
        if dispatch_table_group(command) == "instance"
    )
    device_entries = "\n".join(
        f"  .{command_field_name(str(command['name']))} = {forwarder_entrypoint_name(str(command['name']))},"
        for command in commands
        if dispatch_table_group(command) == "device"
    )
    return f"""#include "generated/forwarder_entrypoints.hpp"

#include "forwarder.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::forwarder::generated {{
namespace {{

const ::vkfwd::generated::GlobalDispatchTable kGlobalDispatchTable {{
  .get_instance_proc_addr = ::vkfwd::Forwarder::getInstanceProcAddr,
{global_entries}
}};

const ::vkfwd::generated::InstanceDispatchTable kInstanceDispatchTable {{
  .get_device_proc_addr = ::vkfwd::Forwarder::getDeviceProcAddr,
{instance_entries}
}};

const ::vkfwd::generated::DeviceDispatchTable kDeviceDispatchTable {{
{device_entries}
}};

}} // namespace

const ::vkfwd::generated::GlobalDispatchTable& global_dispatch_table() {{
  return kGlobalDispatchTable;
}}

const ::vkfwd::generated::InstanceDispatchTable& instance_dispatch_table() {{
  return kInstanceDispatchTable;
}}

const ::vkfwd::generated::DeviceDispatchTable& device_dispatch_table() {{
  return kDeviceDispatchTable;
}}

}} // namespace vkfwd::forwarder::generated
"""


def forwarder_memory_map_command_source_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    """Emit a forwarder entry that fully delegates to MemoryMapForwarder.

    vkMapMemory and vkUnmapMemory cannot use the generated Vulkan command
    payloads because the mapped pointer returned by the receiver is not a valid
    source-process address. The manager sends vkfwd custom memory-map commands
    with explicit staging-transfer payloads instead.
    """
    name = str(command["name"])
    params = command_parameter_declarations(command)
    param_names = command_parameter_names(command)
    ret = command["return_type"]
    entry = forwarder_entrypoint_name(name)
    if str(ret) == "void":
        call = f"::vkfwd::MemoryMapForwarder::instance().custom_{name}_entry({param_names});"
    else:
        call = f"return ::vkfwd::MemoryMapForwarder::instance().custom_{name}_entry({param_names});"
    return f"""#include "generated/forwarder_entrypoints.hpp"

#include "forwarder.hpp"
#include "generated/command/{name}.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/manager.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#if __has_include("hook/{name}ForwarderHook.hpp")
#include "hook/{name}ForwarderHook.hpp"
#endif

namespace vkfwd::forwarder::generated {{

VKAPI_ATTR {ret} VKAPI_CALL {entry}(
    {params}) {{
  // Staging protocol: delegate entirely to the manual MemoryMapForwarder. It
  // emits vkfwd custom memory-map command ids, not generated Vulkan MapMemory /
  // UnmapMemory command payloads.
  {call}
}}

}} // namespace vkfwd::forwarder::generated
"""


# Commands whose public Vulkan forwarder entry delegates to the manual
# MemoryMapForwarder, which emits vkfwd custom command ids instead of the
# generated Vulkan command payload for the same API name.
FORWARDER_MEMORY_MAP_MANAGED_COMMANDS = {"vkMapMemory", "vkUnmapMemory"}

# Receiver endpoint groups, in emission order. Each generated command is assigned
# to exactly one group; endpoint implementations are split into endpoint/<group>.cpp
# so no single source file grows past the ferry-wide grouping limit (see
# src/vkfwd/ferry/README.md "Generator Grouping Policy"). This map is the single
# source of truth a future command/structure grouping migration should also consume.
API_GROUP_ORDER = [
    "global_instance",
    "physical_device",
    "device_lifecycle",
    "memory_buffer",
    "shader_pipeline_layout",
    "renderpass_framebuffer",
    "descriptor",
    "sync_objects",
]

COMMAND_GROUP = {
    "vkEnumerateInstanceVersion": "global_instance",
    "vkEnumerateInstanceLayerProperties": "global_instance",
    "vkEnumerateInstanceExtensionProperties": "global_instance",
    "vkCreateInstance": "global_instance",
    "vkDestroyInstance": "global_instance",
    "vkEnumeratePhysicalDevices": "physical_device",
    "vkGetPhysicalDeviceProperties": "physical_device",
    "vkGetPhysicalDeviceFeatures": "physical_device",
    "vkGetPhysicalDeviceQueueFamilyProperties": "physical_device",
    "vkGetPhysicalDeviceMemoryProperties": "physical_device",
    "vkEnumerateDeviceExtensionProperties": "physical_device",
    "vkCreateDevice": "device_lifecycle",
    "vkDestroyDevice": "device_lifecycle",
    "vkGetDeviceQueue": "device_lifecycle",
    "vkDeviceWaitIdle": "device_lifecycle",
    "vkCreateBuffer": "memory_buffer",
    "vkDestroyBuffer": "memory_buffer",
    "vkGetBufferMemoryRequirements": "memory_buffer",
    "vkAllocateMemory": "memory_buffer",
    "vkFreeMemory": "memory_buffer",
    "vkBindBufferMemory": "memory_buffer",
    "vkMapMemory": "memory_buffer",
    "vkUnmapMemory": "memory_buffer",
    "vkCreateShaderModule": "shader_pipeline_layout",
    "vkDestroyShaderModule": "shader_pipeline_layout",
    "vkCreatePipelineLayout": "shader_pipeline_layout",
    "vkDestroyPipelineLayout": "shader_pipeline_layout",
    "vkCreateGraphicsPipelines": "shader_pipeline_layout",
    "vkDestroyPipeline": "shader_pipeline_layout",
    "vkCreateRenderPass": "renderpass_framebuffer",
    "vkDestroyRenderPass": "renderpass_framebuffer",
    "vkCreateDescriptorSetLayout": "descriptor",
    "vkDestroyDescriptorSetLayout": "descriptor",
    "vkCreateSemaphore": "sync_objects",
    "vkDestroySemaphore": "sync_objects",
}


def command_group(command: dict[str, object]) -> str:
    name = str(command["name"])
    try:
        return COMMAND_GROUP[name]
    except KeyError:
        raise ValueError(
            f"command {name!r} has no receiver endpoint group; add it to COMMAND_GROUP"
        ) from None


def commands_in_group(
    metadata: dict[str, object], group: str
) -> list[dict[str, object]]:
    return [c for c in metadata["commands"] if command_group(c) == group]


def receiver_groups_present(metadata: dict[str, object]) -> list[str]:
    present = {command_group(c) for c in metadata["commands"]}
    return [g for g in API_GROUP_ORDER if g in present]


def forwarder_memory_manager_include(command: dict[str, object]) -> str:
    return ""


def forwarder_command_source_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    if str(command["name"]) in FORWARDER_MEMORY_MAP_MANAGED_COMMANDS:
        return forwarder_memory_map_command_source_content(metadata, command)
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    return_statement = response_return_statement(command)
    failure_return = status_failure_return_statement(command, "status")
    output_assignments = output_parameter_assignments(command)
    if output_assignments:
        output_assignments = "\n" + output_assignments + "\n"
    if command_needs_response(command):
        response_flow = f"""
  CommandStream response_stream = forwarder.flush();
  auto response_view = response_stream.at(0, response_stream.size());
  const Command::Response* packed_response = nullptr;
  status = Command::unpack_response(response_view, &packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{
{failure_return}
  }}
  Command::Response response = *packed_response;

  if constexpr (Hooks::after_response_unpack_enabled) {{
    Hooks::after_response_unpack(parameters, response);
  }}
{output_assignments}
  // Synchronous forwarding flushes this thread's pending request stream and
  // returns a fresh response stream. Generated code only decodes that stream here;
  // transport implementations own delivery, replay, and handle mapping policy.
"""
    elif command_flushes_without_response(command):
        response_flow = f"""
  (void)forwarder.flush();

  // vkDestroyInstance is a lifecycle fence for the source process. It has no
  // response payload, but it must still drain this thread's deferred destroys
  // before the application considers the instance gone.
"""
    else:
        response_flow = f"""
  if constexpr (Hooks::after_pack_enabled) {{
    Hooks::after_pack(parameters);
  }}

  // Deferrable commands have no return value or output parameters, so the
  // entry point only appends to the thread-local request stream. The next
  // non-deferrable command is responsible for flushing this thread's pending
  // command sequence through the transport session.
"""
    return f"""#include "generated/forwarder_entrypoints.hpp"

#include "forwarder.hpp"
#include "generated/command/{command['name']}.hpp"
#include "generated/forwarder_hooks.hpp"
{forwarder_memory_manager_include(command)}

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <algorithm>
#include <cstdint>

#if __has_include("hook/{command['name']}ForwarderHook.hpp")
#include "hook/{command['name']}ForwarderHook.hpp"
#endif

namespace vkfwd::forwarder::generated {{

VKAPI_ATTR {command['return_type']} VKAPI_CALL {forwarder_entrypoint_name(str(command['name']))}(
    {command_parameter_declarations(command)}) {{
  using Command = ::vkfwd::generated::commands::{namespace}::Command;
  using Hooks = ::vkfwd::forwarder::manual::CommandHooks<
      ::vkfwd::generated::CommandId::{enum_name}>;

  if constexpr (Hooks::before_pack_enabled) {{
    Hooks::before_pack({command_parameter_names(command)});
  }}

  auto& forwarder = ::vkfwd::Forwarder::instance();
  Command::Parameters parameters{parameter_initializer_list(command)};
  VkResult status = Command::pack_parameters(forwarder.request_stream(), parameters);
  if (status != VK_SUCCESS) [[unlikely]] {{
{failure_return}
  }}
{response_flow}
{return_statement}
}}

}} // namespace vkfwd::forwarder::generated
"""


def write_dispatch_table_files(metadata: dict[str, object], output_dir: Path) -> None:
    (output_dir / "dispatch_table.hpp").write_text(
        dispatch_table_header_content(metadata), encoding="utf-8"
    )
    (output_dir / "dispatch_table.cpp").write_text(
        dispatch_table_source_content(metadata), encoding="utf-8"
    )


def write_forwarder_files(metadata: dict[str, object], forwarder_dir: Path) -> None:
    entry_dir = forwarder_dir / "entry"
    entry_dir.mkdir(parents=True, exist_ok=True)
    (forwarder_dir / "forwarder_entrypoints.hpp").write_text(
        forwarder_entrypoints_header_content(metadata), encoding="utf-8"
    )
    (forwarder_dir / "forwarder_entrypoints.cpp").write_text(
        forwarder_entrypoints_source_content(metadata), encoding="utf-8"
    )
    (forwarder_dir / "forwarder_hooks.hpp").write_text(
        forwarder_hooks_header_content(metadata), encoding="utf-8"
    )
    for command in metadata["commands"]:
        (entry_dir / f"{command['name']}_entry.cpp").write_text(
            forwarder_command_source_content(metadata, command), encoding="utf-8"
        )


def receiver_response_initializer(command: dict[str, object]) -> str:
    fields = []
    if str(command["return_type"]) != "void":
        fields.append(".return_value = return_value")
    for parameter in command["parameters"]:
        if parameter["direction"] == "output":
            fields.append(f".{parameter['name']} = parameters->{parameter['name']}")
    return "{" + ", ".join(fields) + "}"


def receiver_endpoint_function_name(command: dict[str, object]) -> str:
    return f"{command['name']}_endpoint"


def receiver_endpoint_source(command: dict[str, object]) -> str:
    namespace = command_namespace(str(command["name"]))
    enum_name = command_enum_name(str(command["name"]))
    parameter_names = ", ".join(
        f"parameters->{parameter['name']}" for parameter in command["parameters"]
    )
    pfn_type = command_pfn_type(str(command["name"]))
    fn = receiver_endpoint_function_name(command)

    body: list[str] = []
    if str(command["return_type"]) == "void":
        body.append(f"    api_function({parameter_names});")
    else:
        body.append(
            f"    const {command['return_type']} return_value = api_function({parameter_names});"
        )

    if command_needs_response(command):
        body.append(
            f"    Command::Response response {receiver_response_initializer(command)};"
        )
        body.append(
            "    if constexpr (Hooks::after_call_enabled) { Hooks::after_call(*parameters, response, replay_context); }"
        )
        body.append(
            "    if constexpr (Hooks::before_pack_response_enabled) { Hooks::before_pack_response(*parameters, response, replay_context); }"
        )
        body.append(
            "    const bool packed_ok = Command::pack_response(response_stream, response) == VK_SUCCESS;"
        )
        body.append(
            "    if constexpr (Hooks::after_pack_response_enabled) { Hooks::after_pack_response(*parameters, response_stream); }"
        )
        body.append("    return packed_ok;")
    else:
        body.append("    return true;")

    return f"""bool {fn}(const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                               ::vkfwd::receiver::ReplayContext& replay_context) {{
  using Command = ::vkfwd::generated::commands::{namespace}::Command;
  using Hooks = ::vkfwd::receiver::manual::CommandHooks<::vkfwd::generated::CommandId::{enum_name}>;

  if constexpr (Hooks::replace_endpoint_enabled) {{
    // Full override: the endpoint has no standard unpack/call/pack body
    // (e.g. memory-map delegation where no real Vulkan call happens here).
    return Hooks::replace_endpoint(request_stream, request_range, response_stream, replay_context);
  }} else {{
    const auto raw_function = replay_context.dispatch.getProcByCommandId(::vkfwd::generated::CommandId::{enum_name});
    if (!raw_function) {{ return false; }}
    const auto api_function = reinterpret_cast<{pfn_type}>(raw_function);

    auto& mutable_request_stream = const_cast<CommandStream&>(request_stream);
    auto request_view = mutable_request_stream.at(request_range.offset, request_range.size);

    if constexpr (Hooks::before_unpack_enabled) {{ Hooks::before_unpack(request_view, replay_context); }}

    const Command::Parameters* parameters = nullptr;
    if (Command::unpack_parameters(request_view, &parameters) != VK_SUCCESS) {{ return false; }}

    if constexpr (Hooks::before_call_enabled) {{
      Hooks::before_call(*const_cast<Command::Parameters*>(parameters), replay_context);
    }}
{chr(10).join(body)}
  }}
}}
"""


def receiver_group_header_content(metadata: dict[str, object], group: str) -> str:
    commands = commands_in_group(metadata, group)
    declarations = "\n".join(
        "bool "
        f"{receiver_endpoint_function_name(command)}(const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream, "
        "::vkfwd::receiver::ReplayContext& replay_context);"
        for command in commands
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"
#include "generated/dispatch_table.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::generated {{

{declarations}

}} // namespace vkfwd::receiver::generated
"""


def receiver_group_source_content(metadata: dict[str, object], group: str) -> str:
    commands = commands_in_group(metadata, group)
    includes = "\n".join(
        f'#include "generated/command/{command["name"]}.hpp"' for command in commands
    )
    hook_includes = "\n".join(
        f'#if __has_include("hook/{command["name"]}ReceiverHook.hpp")\n'
        f'#include "hook/{command["name"]}ReceiverHook.hpp"\n'
        f"#endif"
        for command in commands
    )
    endpoints = "\n".join(receiver_endpoint_source(command) for command in commands)
    return f"""#include "generated/endpoint/{group}.hpp"

{includes}
#include "generated/receiver_hooks.hpp"
#include "memory_map/manager.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

{hook_includes}

namespace vkfwd::receiver::generated {{

{endpoints}
}} // namespace vkfwd::receiver::generated
"""


def receiver_endpoint_dispatch_cases(metadata: dict[str, object]) -> str:
    return "\n".join(
        f"  case ::vkfwd::generated::CommandId::{command_enum_name(str(command['name']))}:\n"
        f"    return {receiver_endpoint_function_name(command)}(request_stream, request_range, response_stream, replay_context);"
        for command in metadata["commands"]
    )


def receiver_endpoints_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"
#include "generated/dispatch_table.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::generated {{

bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                       ::vkfwd::receiver::ReplayContext& replay_context);

}} // namespace vkfwd::receiver::generated
"""


def receiver_endpoints_source_content(metadata: dict[str, object]) -> str:
    group_includes = "\n".join(
        f'#include "generated/endpoint/{group}.hpp"'
        for group in receiver_groups_present(metadata)
    )
    dispatch_cases = receiver_endpoint_dispatch_cases(metadata)
    return f"""#include "generated/endpoints.hpp"

{group_includes}

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::receiver::generated {{

bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                       ::vkfwd::receiver::ReplayContext& replay_context) {{
  switch (command_id) {{
{dispatch_cases}
  }}
  return false;
}}

}} // namespace vkfwd::receiver::generated
"""


def write_receiver_files(metadata: dict[str, object], receiver_dir: Path) -> None:
    receiver_dir.mkdir(parents=True, exist_ok=True)
    endpoint_dir = receiver_dir / "endpoint"
    endpoint_dir.mkdir(parents=True, exist_ok=True)

    (receiver_dir / "receiver_hooks.hpp").write_text(
        receiver_hooks_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.hpp").write_text(
        receiver_endpoints_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.cpp").write_text(
        receiver_endpoints_source_content(metadata), encoding="utf-8"
    )

    present = receiver_groups_present(metadata)
    # Remove stale generated group files so a shrinking manifest never leaves
    # an orphaned .cpp that the CMake glob would still compile.
    for existing in list(endpoint_dir.glob("*.hpp")) + list(endpoint_dir.glob("*.cpp")):
        if existing.stem not in present:
            existing.unlink()
    for group in present:
        (endpoint_dir / f"{group}.hpp").write_text(
            receiver_group_header_content(metadata, group), encoding="utf-8"
        )
        (endpoint_dir / f"{group}.cpp").write_text(
            receiver_group_source_content(metadata, group), encoding="utf-8"
        )


def structure_test_support_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace vkfwd::generated::structure::test {{

template<class Pointer>
std::size_t encoded_offset(Pointer pointer) {{
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(pointer));
}}

inline SafeArrayView<std::uint8_t> view_from(CommandStream & stream, std::size_t offset) {{
    return stream.at(offset, stream.size() - offset);
}}

template<class Pointer>
bool points_into(SafeArrayView<std::uint8_t> & view, Pointer pointer) {{
    auto * begin = view.address(0);
    if (!begin || !pointer) {{ return false; }}
    const auto * target = reinterpret_cast<const std::uint8_t *>(pointer);
    return target >= begin && target < begin + view.size();
}}

template<class Pointer>
bool points_into_blob(CommandStream & stream, Pointer pointer) {{
    auto view = stream.at(0, stream.size());
    return points_into(view, pointer);
}}

template<class T>
const T & object_at(const CommandStream & stream, std::size_t offset) {{
    const auto view = stream.at(offset, sizeof(T));
    REQUIRE(!view.empty());
    return *reinterpret_cast<const T *>(&view.at(0));
}}

inline void check_relative_string(CommandStream & stream, std::size_t, const char * value, std::string_view expected) {{
    REQUIRE(value != nullptr);
    CHECK(points_into_blob(stream, value));
    CHECK(std::string_view(value, expected.size()) == expected);
    CHECK(value[expected.size()] == '\\0');
}}

inline void check_relative_string_array(CommandStream & stream, std::size_t base_offset, const char * const * encoded_values,
                                        std::initializer_list<std::string_view> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    CHECK(points_into_blob(stream, encoded_values));
    std::size_t index = 0;
    for (std::string_view expected_value : expected) {{
        const auto * actual_value = encoded_values[index];
        REQUIRE(actual_value != nullptr);
        CHECK(points_into_blob(stream, actual_value));
        CHECK(std::string_view(actual_value, expected_value.size()) == expected_value);
        CHECK(actual_value[expected_value.size()] == '\\0');
        ++index;
    }}
}}

template<class T>
void check_relative_plain_array(CommandStream & stream, std::size_t base_offset, const T * encoded_values, std::initializer_list<T> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    CHECK(points_into_blob(stream, encoded_values));
    std::size_t index = 0;
    for (const T & expected_value : expected) {{
        CHECK(encoded_values[index] == expected_value);
        ++index;
    }}
}}

template<class Handle>
Handle test_handle(std::uintptr_t value) {{
    return reinterpret_cast<Handle>(value);
}}

}} // namespace vkfwd::generated::structure::test
"""


def application_instance_structure_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::generated::structure::test {{
namespace {{

TEST_CASE("VkApplicationInfo generated structure pack/unpack preserves copied strings") {{
    CommandStream stream;
    PackedStruct packed;
    VkApplicationInfo value {{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "vkfwd-structure-app",
        .applicationVersion = 3,
        .pEngineName        = "vkfwd-structure-engine",
        .engineVersion      = 5,
        .apiVersion         = VK_MAKE_API_VERSION(0, 1, 4, 0),
    }};

    REQUIRE(pack_VkApplicationInfo(&value, stream, packed) == VK_SUCCESS);
    const VkApplicationInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkApplicationInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->applicationVersion == value.applicationVersion);
    CHECK(actual->engineVersion == value.engineVersion);
    CHECK(actual->apiVersion == value.apiVersion);
    check_relative_string(flattened, packed.offset, actual->pApplicationName, value.pApplicationName);
    check_relative_string(flattened, packed.offset, actual->pEngineName, value.pEngineName);
}}

TEST_CASE("VkInstanceCreateInfo generated structure pack/unpack preserves nested application info and name arrays") {{
    CommandStream stream;
    PackedStruct packed;
    VkApplicationInfo app {{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "vkfwd-instance-app",
        .applicationVersion = 7,
        .pEngineName        = "vkfwd-instance-engine",
        .engineVersion      = 11,
        .apiVersion         = VK_MAKE_API_VERSION(0, 1, 3, 0),
    }};
    std::array<const char *, 2> layers {{"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"}};
    std::array<const char *, 2> extensions {{"VK_EXT_debug_utils", "VK_KHR_surface"}};
    VkInstanceCreateInfo value {{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkInstanceCreateFlags {{0x4}},
        .pApplicationInfo        = &app,
        .enabledLayerCount       = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames     = layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    }};

    REQUIRE(pack_VkInstanceCreateInfo(&value, stream, packed) == VK_SUCCESS);
    const VkInstanceCreateInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkInstanceCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->enabledLayerCount == value.enabledLayerCount);
    CHECK(actual->enabledExtensionCount == value.enabledExtensionCount);
    check_relative_string_array(flattened, packed.offset, actual->ppEnabledLayerNames, {{"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"}});
    check_relative_string_array(flattened, packed.offset, actual->ppEnabledExtensionNames, {{"VK_EXT_debug_utils", "VK_KHR_surface"}});

    REQUIRE(actual->pApplicationInfo != nullptr);
    const VkApplicationInfo * actual_app = actual->pApplicationInfo;
    REQUIRE(actual_app != nullptr);
    CHECK(points_into_blob(flattened, actual_app));
    CHECK(actual_app->applicationVersion == app.applicationVersion);
    CHECK(actual_app->engineVersion == app.engineVersion);
    check_relative_string(flattened, 0, actual_app->pApplicationName, app.pApplicationName);
    check_relative_string(flattened, 0, actual_app->pEngineName, app.pEngineName);
}}

}} // namespace
}} // namespace vkfwd::generated::structure::test
"""


def device_structure_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::generated::structure::test {{
namespace {{

TEST_CASE("VkDeviceQueueCreateInfo generated structure pack/unpack preserves priority arrays") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<float, 2> priorities {{0.25f, 0.75f}};
    VkDeviceQueueCreateInfo value {{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VkDeviceQueueCreateFlags {{0x2}},
        .queueFamilyIndex = 3,
        .queueCount       = static_cast<std::uint32_t>(priorities.size()),
        .pQueuePriorities = priorities.data(),
    }};

    REQUIRE(pack_VkDeviceQueueCreateInfo(&value, stream, packed) == VK_SUCCESS);
    const VkDeviceQueueCreateInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkDeviceQueueCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->queueFamilyIndex == value.queueFamilyIndex);
    CHECK(actual->queueCount == value.queueCount);
    check_relative_plain_array(flattened, packed.offset, actual->pQueuePriorities, {{0.25f, 0.75f}});
}}

TEST_CASE("VkDeviceCreateInfo generated structure pack/unpack preserves nested queue info, names, and features") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<float, 2> priorities {{0.5f, 1.0f}};
    VkDeviceQueueCreateInfo queue {{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VkDeviceQueueCreateFlags {{0x1}},
        .queueFamilyIndex = 9,
        .queueCount       = static_cast<std::uint32_t>(priorities.size()),
        .pQueuePriorities = priorities.data(),
    }};
    std::array<const char *, 1> layers {{"VK_LAYER_VKFWD_device"}};
    std::array<const char *, 2> extensions {{"VK_KHR_swapchain", "VK_EXT_private_data"}};
    VkPhysicalDeviceFeatures features {{}};
    features.robustBufferAccess = VK_TRUE;
    features.samplerAnisotropy  = VK_TRUE;
    VkDeviceCreateInfo value {{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkDeviceCreateFlags {{0x8}},
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queue,
        .enabledLayerCount       = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames     = layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
        .pEnabledFeatures        = &features,
    }};

    REQUIRE(pack_VkDeviceCreateInfo(&value, stream, packed) == VK_SUCCESS);
    const VkDeviceCreateInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkDeviceCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->queueCreateInfoCount == value.queueCreateInfoCount);
    CHECK(actual->enabledLayerCount == value.enabledLayerCount);
    CHECK(actual->enabledExtensionCount == value.enabledExtensionCount);
    check_relative_string_array(flattened, packed.offset, actual->ppEnabledLayerNames, {{"VK_LAYER_VKFWD_device"}});
    check_relative_string_array(flattened, packed.offset, actual->ppEnabledExtensionNames, {{"VK_KHR_swapchain", "VK_EXT_private_data"}});

    REQUIRE(actual->pQueueCreateInfos != nullptr);
    const VkDeviceQueueCreateInfo * actual_queue = actual->pQueueCreateInfos;
    REQUIRE(actual_queue != nullptr);
    CHECK(points_into_blob(flattened, actual_queue));
    CHECK(actual_queue->queueFamilyIndex == queue.queueFamilyIndex);
    CHECK(actual_queue->queueCount == queue.queueCount);
    check_relative_plain_array(flattened, 0, actual_queue->pQueuePriorities, {{0.5f, 1.0f}});

    REQUIRE(actual->pEnabledFeatures != nullptr);
    CHECK(points_into_blob(flattened, actual->pEnabledFeatures));
    const auto & actual_features = *actual->pEnabledFeatures;
    CHECK(actual_features.robustBufferAccess == VK_TRUE);
    CHECK(actual_features.samplerAnisotropy == VK_TRUE);
}}

TEST_CASE("VkDeviceGroupDeviceCreateInfo generated structure pack/unpack preserves physical device arrays") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<VkPhysicalDevice, 2> devices {{
        test_handle<VkPhysicalDevice>(0x101),
        test_handle<VkPhysicalDevice>(0x202),
    }};
    VkDeviceGroupDeviceCreateInfo value {{
        .sType               = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
        .pNext               = nullptr,
        .physicalDeviceCount = static_cast<std::uint32_t>(devices.size()),
        .pPhysicalDevices    = devices.data(),
    }};

    REQUIRE(pack_VkDeviceGroupDeviceCreateInfo(&value, stream, packed) == VK_SUCCESS);
    const VkDeviceGroupDeviceCreateInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkDeviceGroupDeviceCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->physicalDeviceCount == value.physicalDeviceCount);
    check_relative_plain_array(flattened, packed.offset, actual->pPhysicalDevices,
                               {{test_handle<VkPhysicalDevice>(0x101), test_handle<VkPhysicalDevice>(0x202)}});
}}

TEST_CASE("VkDeviceQueueGlobalPriorityCreateInfo generated structure pack/unpack preserves global priority") {{
    CommandStream stream;
    PackedStruct packed;
    VkDeviceQueueGlobalPriorityCreateInfo value {{
        .sType          = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,
        .pNext          = nullptr,
        .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH,
    }};

    REQUIRE(pack_VkDeviceQueueGlobalPriorityCreateInfo(&value, stream, packed) == VK_SUCCESS);
    const VkDeviceQueueGlobalPriorityCreateInfo * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkDeviceQueueGlobalPriorityCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->globalPriority == value.globalPriority);
}}

}} // namespace
}} // namespace vkfwd::generated::structure::test
"""


def physical_device_features_structure_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::generated::structure::test {{
namespace {{

TEST_CASE("VkPhysicalDeviceFeatures2 generated structure pack/unpack preserves feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceFeatures2 value {{
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = nullptr,
        .features = {{}},
    }};
    value.features.robustBufferAccess = VK_TRUE;
    value.features.geometryShader     = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceFeatures2(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceFeatures2 * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceFeatures2(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->features.robustBufferAccess == VK_TRUE);
    CHECK(actual->features.geometryShader == VK_TRUE);
}}

TEST_CASE("VkPhysicalDeviceVulkan11Features generated structure pack/unpack preserves selected feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceVulkan11Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = nullptr,
    }};
    value.storageBuffer16BitAccess = VK_TRUE;
    value.shaderDrawParameters     = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan11Features(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan11Features * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceVulkan11Features(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->storageBuffer16BitAccess == VK_TRUE);
    CHECK(actual->shaderDrawParameters == VK_TRUE);
}}

TEST_CASE("VkPhysicalDeviceVulkan12Features generated structure pack/unpack preserves selected feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceVulkan12Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = nullptr,
    }};
    value.descriptorIndexing = VK_TRUE;
    value.timelineSemaphore  = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan12Features(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan12Features * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceVulkan12Features(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->descriptorIndexing == VK_TRUE);
    CHECK(actual->timelineSemaphore == VK_TRUE);
}}

TEST_CASE("VkPhysicalDeviceVulkan13Features generated structure pack/unpack preserves selected feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceVulkan13Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = nullptr,
    }};
    value.synchronization2  = VK_TRUE;
    value.dynamicRendering = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan13Features(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan13Features * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceVulkan13Features(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->synchronization2 == VK_TRUE);
    CHECK(actual->dynamicRendering == VK_TRUE);
}}

TEST_CASE("VkPhysicalDeviceVulkan14Features generated structure pack/unpack preserves selected feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceVulkan14Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = nullptr,
    }};
    value.globalPriorityQuery = VK_TRUE;
    value.maintenance6        = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan14Features(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan14Features * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceVulkan14Features(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->globalPriorityQuery == VK_TRUE);
    CHECK(actual->maintenance6 == VK_TRUE);
}}

TEST_CASE("VkPhysicalDeviceDescriptorIndexingFeatures generated structure pack/unpack preserves selected feature bits") {{
    CommandStream stream;
    PackedStruct packed;
    VkPhysicalDeviceDescriptorIndexingFeatures value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .pNext = nullptr,
    }};
    value.descriptorBindingPartiallyBound        = VK_TRUE;
    value.descriptorBindingVariableDescriptorCount = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceDescriptorIndexingFeatures(&value, stream, packed) == VK_SUCCESS);
    const VkPhysicalDeviceDescriptorIndexingFeatures * actual = nullptr;
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    REQUIRE(unpack_VkPhysicalDeviceDescriptorIndexingFeatures(view, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->descriptorBindingPartiallyBound == VK_TRUE);
    CHECK(actual->descriptorBindingVariableDescriptorCount == VK_TRUE);
}}

}} // namespace
}} // namespace vkfwd::generated::structure::test
"""


def command_structs_structure_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/structure/command_structs.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace vkfwd::generated::structure::test {{
namespace {{

TEST_CASE("VkBufferCreateInfo generated command-structure pack/unpack preserves queue family indices") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<std::uint32_t, 2> families {{2, 5}};
    VkBufferCreateInfo value {{
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = VkBufferCreateFlags {{0x1}},
        .size                  = 4096,
        .usage                 = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode           = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = static_cast<std::uint32_t>(families.size()),
        .pQueueFamilyIndices   = families.data(),
    }};

    REQUIRE(pack_VkBufferCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkBufferCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkBufferCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    CHECK(actual->size == value.size);
    check_relative_plain_array(flattened, packed.offset, actual->pQueueFamilyIndices, {{2u, 5u}});
}}

TEST_CASE("VkMemoryAllocateInfo generated command-structure pack/unpack preserves scalar allocation fields") {{
    CommandStream stream;
    PackedStruct packed;
    VkMemoryAllocateInfo value {{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = 8192,
        .memoryTypeIndex = 3,
    }};
    REQUIRE(pack_VkMemoryAllocateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkMemoryAllocateInfo * actual = nullptr;
    REQUIRE(unpack_VkMemoryAllocateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    CHECK(actual->allocationSize == value.allocationSize);
    CHECK(actual->memoryTypeIndex == value.memoryTypeIndex);
}}

TEST_CASE("VkShaderModuleCreateInfo generated command-structure pack/unpack preserves SPIR-V words") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<std::uint32_t, 4> code {{0x07230203u, 0x00010000u, 0x0008000au, 0u}};
    VkShaderModuleCreateInfo value {{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = VkShaderModuleCreateFlags {{0}},
        .codeSize = code.size() * sizeof(std::uint32_t),
        .pCode    = code.data(),
    }};
    REQUIRE(pack_VkShaderModuleCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkShaderModuleCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkShaderModuleCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    CHECK(actual->codeSize == value.codeSize);
    check_relative_plain_array(flattened, packed.offset, actual->pCode, {{code[0], code[1], code[2], code[3]}});
}}

TEST_CASE("VkDescriptorSetLayoutCreateInfo generated command-structure pack/unpack preserves bindings") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<VkDescriptorSetLayoutBinding, 1> bindings {{
        VkDescriptorSetLayoutBinding {{.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}},
    }};
    VkDescriptorSetLayoutCreateInfo value {{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = nullptr,
        .flags        = VkDescriptorSetLayoutCreateFlags {{0}},
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings    = bindings.data(),
    }};
    REQUIRE(pack_VkDescriptorSetLayoutCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkDescriptorSetLayoutCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkDescriptorSetLayoutCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    REQUIRE(actual->pBindings != nullptr);
    CHECK(points_into_blob(flattened, actual->pBindings));
    CHECK(actual->pBindings[0].binding == 2);
}}

TEST_CASE("VkPipelineLayoutCreateInfo generated command-structure pack/unpack preserves set layouts and push constants") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<VkDescriptorSetLayout, 1> layouts {{test_handle<VkDescriptorSetLayout>(0x101)}};
    std::array<VkPushConstantRange, 1> ranges {{VkPushConstantRange {{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 4, .size = 8}}}};
    VkPipelineLayoutCreateInfo value {{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext                  = nullptr,
        .flags                  = VkPipelineLayoutCreateFlags {{0}},
        .setLayoutCount         = static_cast<std::uint32_t>(layouts.size()),
        .pSetLayouts            = layouts.data(),
        .pushConstantRangeCount = static_cast<std::uint32_t>(ranges.size()),
        .pPushConstantRanges    = ranges.data(),
    }};
    REQUIRE(pack_VkPipelineLayoutCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkPipelineLayoutCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkPipelineLayoutCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    CHECK(actual->pSetLayouts[0] == layouts[0]);
    CHECK(actual->pPushConstantRanges[0].size == 8);
}}

TEST_CASE("VkRenderPassCreateInfo generated command-structure pack/unpack preserves subpass attachment arrays") {{
    CommandStream stream;
    PackedStruct packed;
    std::array<VkAttachmentDescription, 1> attachments {{VkAttachmentDescription {{.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}}}};
    std::array<VkAttachmentReference, 1> colors {{VkAttachmentReference {{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}}};
    std::array<VkSubpassDescription, 1> subpasses {{VkSubpassDescription {{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = colors.data()}}}};
    VkRenderPassCreateInfo value {{
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext           = nullptr,
        .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
        .pAttachments    = attachments.data(),
        .subpassCount    = static_cast<std::uint32_t>(subpasses.size()),
        .pSubpasses      = subpasses.data(),
    }};
    REQUIRE(pack_VkRenderPassCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkRenderPassCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkRenderPassCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    REQUIRE(actual->pSubpasses != nullptr);
    REQUIRE(actual->pSubpasses[0].pColorAttachments != nullptr);
    CHECK(actual->pSubpasses[0].pColorAttachments[0].attachment == 0);
}}

TEST_CASE("VkSemaphoreCreateInfo generated command-structure pack/unpack preserves scalar fields") {{
    CommandStream stream;
    PackedStruct packed;
    VkSemaphoreCreateInfo value {{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VkSemaphoreCreateFlags {{0}},
    }};
    REQUIRE(pack_VkSemaphoreCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkSemaphoreCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkSemaphoreCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    CHECK(actual->sType == value.sType);
}}

TEST_CASE("VkGraphicsPipelineCreateInfo generated command-structure pack/unpack preserves nested pipeline arrays") {{
    CommandStream stream;
    PackedStruct packed;
    VkPipelineShaderStageCreateInfo stage {{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext  = nullptr,
        .stage  = VK_SHADER_STAGE_VERTEX_BIT,
        .module = test_handle<VkShaderModule>(0x202),
        .pName  = "main",
    }};
    std::array<VkVertexInputBindingDescription, 1> bindings {{VkVertexInputBindingDescription {{.binding = 0, .stride = 8}}}};
    std::array<VkVertexInputAttributeDescription, 1> attributes {{VkVertexInputAttributeDescription {{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT}}}};
    VkPipelineVertexInputStateCreateInfo vertex {{
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = static_cast<std::uint32_t>(bindings.size()),
        .pVertexBindingDescriptions      = bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions    = attributes.data(),
    }};
    VkPipelineInputAssemblyStateCreateInfo assembly {{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST}};
    std::array<VkDynamicState, 1> dynamic_states {{VK_DYNAMIC_STATE_VIEWPORT}};
    VkPipelineDynamicStateCreateInfo dynamic {{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = dynamic_states.data()}};
    VkGraphicsPipelineCreateInfo value {{
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = nullptr,
        .stageCount          = 1,
        .pStages             = &stage,
        .pVertexInputState   = &vertex,
        .pInputAssemblyState = &assembly,
        .pDynamicState       = &dynamic,
        .layout              = test_handle<VkPipelineLayout>(0x303),
        .renderPass          = test_handle<VkRenderPass>(0x404),
    }};
    REQUIRE(pack_VkGraphicsPipelineCreateInfo(&value, stream, packed) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto view = view_from(flattened, packed.offset);
    const VkGraphicsPipelineCreateInfo * actual = nullptr;
    REQUIRE(unpack_VkGraphicsPipelineCreateInfo(view, &actual) == VK_SUCCESS);
    REQUIRE(points_into(view, actual));
    REQUIRE(actual->pStages != nullptr);
    check_relative_string(flattened, packed.offset, actual->pStages[0].pName, "main");
    REQUIRE(actual->pVertexInputState != nullptr);
    CHECK(actual->pVertexInputState->pVertexBindingDescriptions[0].stride == 8);
    REQUIRE(actual->pDynamicState != nullptr);
    CHECK(actual->pDynamicState->pDynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT);
}}

}} // namespace
}} // namespace vkfwd::generated::structure::test
"""


def write_structure_test_files(metadata: dict[str, object], output_dir: Path) -> None:
    test_dir = output_dir / "structure" / "test"
    test_dir.mkdir(parents=True, exist_ok=True)
    (test_dir / "support.hpp").write_text(
        structure_test_support_content(metadata), encoding="utf-8"
    )

    sources = {
        "application_instance_structure_test.cpp": application_instance_structure_test_content(
            metadata
        ),
        "device_structure_test.cpp": device_structure_test_content(metadata),
        "physical_device_features_structure_test.cpp": physical_device_features_structure_test_content(
            metadata
        ),
        "command_structs_structure_test.cpp": command_structs_structure_test_content(
            metadata
        ),
    }
    for file_name, content in sources.items():
        (test_dir / file_name).write_text(content, encoding="utf-8")

    manifest_sources = "\n".join(f"  {source}" for source in sorted(sources))
    (test_dir / "internal-test.cmake").write_text(
        f"""# This generated manifest is consumed by dev/test/internal-test/CMakeLists.txt.
# Keep entries relative so generated structure tests stay beside their helpers.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
{manifest_sources})
""",
        encoding="utf-8",
    )


def command_structs_header_content(metadata: dict[str, object]) -> str:
    declarations = "\n".join(
        f"VkResult pack_{name}(const {name}* value, CommandStream& stream, PackedStruct& packed);\n"
        f"VkResult pack_array_{name}(const {name}* values, std::uint32_t count, CommandStream& stream, PackedStruct& packed);\n"
        f"VkResult unpack_{name}(SafeArrayView<std::uint8_t>& view, const {name}** value);\n"
        f"VkResult unpack_array_{name}(SafeArrayView<std::uint8_t>& view, std::uint32_t count, const {name}** values);"
        for name in sorted(
            SUPPORTED_COMMAND_STRUCT_PARAMETERS
            - {"VkInstanceCreateInfo", "VkDeviceCreateInfo"}
        )
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"
#include "generated/structure/core.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkfwd::generated::structure {{

{declarations}

}} // namespace vkfwd::generated::structure
"""


def command_structs_source_content(metadata: dict[str, object]) -> str:
    content = """#include "generated/structure/command_structs.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: @VULKAN_API_VERSION@
// Vulkan XML SHA256: @VK_XML_SHA256@

#include "logging.hpp"

#include <cstddef>
#include <cstring>
#include <new>

namespace vkfwd::generated::structure {{
namespace {{

template<class T>
VkResult append_shallow_struct(const T* value, CommandStream& stream, PackedStruct& packed, T*& packed_value) {{
  packed_value = nullptr;
  if (!value) {{
    packed.offset = 0;
    return VK_SUCCESS;
  }}
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<T>(1, alignof(T), &target);
    if (!destination.set(0, *value)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: could not copy shallow struct, size={{}}", sizeof(T));
      return VK_ERROR_UNKNOWN;
    }}
    packed.offset = target;
    packed_value = &destination.at(0);
    return VK_SUCCESS;
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: out of host memory copying shallow struct, size={{}}", sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

template<class Pointer>
VkResult patch_pointer(Pointer& pointer_slot, std::size_t pointer_slot_offset, std::size_t target_offset) {{
  // Command create-info structs use the same field-relative pointer encoding as
  // core structures so nested source pointers never cross the receiver boundary.
  pointer_slot = reinterpret_cast<Pointer>(target_offset ? static_cast<std::uintptr_t>(target_offset - pointer_slot_offset) : 0);
  return VK_SUCCESS;
}}

template<class Pointer>
VkResult recover_pointer(Pointer& pointer_slot, SafeArrayView<std::uint8_t>& view) {{
  if (!pointer_slot) {{ return VK_SUCCESS; }}
  auto* begin = view.address(0);
  if (!begin) [[unlikely]] {{ return VK_ERROR_UNKNOWN; }}
  auto* slot = reinterpret_cast<std::uint8_t*>(&pointer_slot);
  auto* end = begin + view.size();
  auto* target = slot + reinterpret_cast<std::uintptr_t>(pointer_slot);
  if (slot < begin || slot + sizeof(Pointer) > end || target < begin || target >= end) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command-structure unpack failed: encoded pointer is outside view, view_size={{}}", view.size());
    return VK_ERROR_UNKNOWN;
  }}
  pointer_slot = reinterpret_cast<Pointer>(target);
  return VK_SUCCESS;
}}

SafeArrayView<std::uint8_t> tail_view_from_pointer(SafeArrayView<std::uint8_t>& view, const void* pointer) {{
  auto* begin = view.address(0);
  if (!begin || !pointer) {{ return {{}}; }}
  auto* target = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(pointer));
  auto* end = begin + view.size();
  if (target < begin || target >= end) {{ return {{}}; }}
  return SafeArrayView<std::uint8_t>(static_cast<std::size_t>(end - target), target);
}}

template<class T>
VkResult pack_plain_array(const T* values, std::uint32_t count, CommandStream& stream, std::size_t slot_offset, const T*& pointer_slot) {{
  if (!values || count == 0) {{ return patch_pointer(pointer_slot, slot_offset, 0); }}
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<T>(count, alignof(T), &target);
    if (destination.set(0, count, values) != count) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: could not copy array, count={{}}, element_size={{}}", count, sizeof(T));
      return VK_ERROR_UNKNOWN;
    }}
    return patch_pointer(pointer_slot, slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: out of host memory copying array, count={{}}, element_size={{}}", count, sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

template<class T>
VkResult pack_plain_object(const T* value, CommandStream& stream, std::size_t slot_offset, const T*& pointer_slot, std::size_t& object_offset,
                           T*& packed_object) {{
  object_offset = 0;
  packed_object = nullptr;
  if (!value) {{ return patch_pointer(pointer_slot, slot_offset, 0); }}
  try {{
    auto destination = stream.grow<T>(1, alignof(T), &object_offset);
    if (!destination.set(0, *value)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: could not copy nested object, size={{}}", sizeof(T));
      return VK_ERROR_UNKNOWN;
    }}
    packed_object = &destination.at(0);
    return patch_pointer(pointer_slot, slot_offset, object_offset);
  }} catch (const std::bad_alloc&) {{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

VkResult pack_string(const char* value, CommandStream& stream, std::size_t slot_offset, const char*& pointer_slot) {{
  if (!value) {{ return patch_pointer(pointer_slot, slot_offset, 0); }}
  try {{
    const std::size_t size = std::strlen(value) + 1;
    std::size_t target = 0;
    auto destination = stream.grow<char>(size, alignof(char), &target);
    if (destination.set(0, size, value) != size) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command-structure pack failed: could not copy string");
      return VK_ERROR_UNKNOWN;
    }}
    return patch_pointer(pointer_slot, slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

VkResult recover_pnext(const void*& pnext, SafeArrayView<std::uint8_t>& view) {{
  VkResult status = recover_pointer(pnext, view);
  if (status != VK_SUCCESS || !pnext) {{ return status; }}
  auto child_view = tail_view_from_pointer(view, pnext);
  const void* ignored = nullptr;
  return unpack_pnext_chain(child_view, &ignored);
}}

VkResult pack_pnext(const void* pnext, CommandStream& stream, std::size_t slot_offset, const void*& pointer_slot) {{
  PackedStruct packed_pnext;
  VkResult status = pack_pnext_chain(pnext, stream, packed_pnext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return patch_pointer(pointer_slot, slot_offset, packed_pnext.offset);
}}

VkResult pack_specialization_info(const VkSpecializationInfo* value, CommandStream& stream, std::size_t slot_offset,
                                  const VkSpecializationInfo*& pointer_slot) {{
  VkSpecializationInfo* packed_value = nullptr;
  PackedStruct packed;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_plain_array(value->pMapEntries, value->mapEntryCount, stream, packed.offset + offsetof(VkSpecializationInfo, pMapEntries),
                            packed_value->pMapEntries);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(reinterpret_cast<const std::uint8_t*>(value->pData), static_cast<std::uint32_t>(value->dataSize), stream,
                            packed.offset + offsetof(VkSpecializationInfo, pData), reinterpret_cast<const std::uint8_t*&>(packed_value->pData));
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return patch_pointer(pointer_slot, slot_offset, packed.offset);
}}

VkResult unpack_specialization_info(SafeArrayView<std::uint8_t>& view, const VkSpecializationInfo** value) {{
  auto* typed = view.size() < sizeof(VkSpecializationInfo) ? nullptr : reinterpret_cast<VkSpecializationInfo*>(view.address(0));
  if (!typed) [[unlikely]] {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pointer(typed->pMapEntries, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pData, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkPipelineShaderStageCreateInfo_payload(const VkPipelineShaderStageCreateInfo& source, VkPipelineShaderStageCreateInfo& packed,
                                                      CommandStream& stream, std::size_t base_offset) {{
  VkResult status = pack_pnext(source.pNext, stream, base_offset + offsetof(VkPipelineShaderStageCreateInfo, pNext), packed.pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_string(source.pName, stream, base_offset + offsetof(VkPipelineShaderStageCreateInfo, pName), packed.pName);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_specialization_info(source.pSpecializationInfo, stream, base_offset + offsetof(VkPipelineShaderStageCreateInfo, pSpecializationInfo),
                                  packed.pSpecializationInfo);
}}

VkResult unpack_VkPipelineShaderStageCreateInfo_payload(SafeArrayView<std::uint8_t>& view, VkPipelineShaderStageCreateInfo& typed) {{
  VkResult status = recover_pnext(typed.pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pName, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pSpecializationInfo, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (typed.pSpecializationInfo) {{
    auto child = tail_view_from_pointer(view, typed.pSpecializationInfo);
    const VkSpecializationInfo* ignored = nullptr;
    status = unpack_specialization_info(child, &ignored);
  }}
  return status;
}}

template<class T, class Patch>
VkResult pack_nested_struct_array(const T* values, std::uint32_t count, CommandStream& stream, std::size_t slot_offset, const T*& pointer_slot,
                                  Patch patch) {{
  if (!values || count == 0) {{ return patch_pointer(pointer_slot, slot_offset, 0); }}
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<T>(count, alignof(T), &target);
    if (destination.set(0, count, values) != count) [[unlikely]] {{ return VK_ERROR_UNKNOWN; }}
    for (std::uint32_t i = 0; i < count; ++i) {{
      VkResult status = patch(values[i], destination.at(i), stream, target + i * sizeof(T));
      if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    }}
    return patch_pointer(pointer_slot, slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

VkResult pack_VkGraphicsPipelineCreateInfo_payload(const VkGraphicsPipelineCreateInfo& source, VkGraphicsPipelineCreateInfo& packed,
                                                   CommandStream& stream, std::size_t base_offset);
VkResult unpack_VkGraphicsPipelineCreateInfo_payload(SafeArrayView<std::uint8_t>& view, VkGraphicsPipelineCreateInfo& typed);

}} // namespace

VkResult pack_VkBufferCreateInfo(const VkBufferCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkBufferCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_pnext(value->pNext, stream, packed.offset + offsetof(VkBufferCreateInfo, pNext), packed_value->pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_plain_array(value->pQueueFamilyIndices, value->queueFamilyIndexCount, stream,
                          packed.offset + offsetof(VkBufferCreateInfo, pQueueFamilyIndices), packed_value->pQueueFamilyIndices);
}}

VkResult unpack_VkBufferCreateInfo(SafeArrayView<std::uint8_t>& view, const VkBufferCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkBufferCreateInfo) ? nullptr : reinterpret_cast<VkBufferCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pQueueFamilyIndices, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkMemoryAllocateInfo(const VkMemoryAllocateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkMemoryAllocateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  return pack_pnext(value->pNext, stream, packed.offset + offsetof(VkMemoryAllocateInfo, pNext), packed_value->pNext);
}}

VkResult unpack_VkMemoryAllocateInfo(SafeArrayView<std::uint8_t>& view, const VkMemoryAllocateInfo** value) {{
  auto* typed = view.size() < sizeof(VkMemoryAllocateInfo) ? nullptr : reinterpret_cast<VkMemoryAllocateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkShaderModuleCreateInfo(const VkShaderModuleCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkShaderModuleCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_pnext(value->pNext, stream, packed.offset + offsetof(VkShaderModuleCreateInfo, pNext), packed_value->pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_plain_array(value->pCode, static_cast<std::uint32_t>(value->codeSize / sizeof(std::uint32_t)), stream,
                          packed.offset + offsetof(VkShaderModuleCreateInfo, pCode), packed_value->pCode);
}}

VkResult unpack_VkShaderModuleCreateInfo(SafeArrayView<std::uint8_t>& view, const VkShaderModuleCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkShaderModuleCreateInfo) ? nullptr : reinterpret_cast<VkShaderModuleCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pCode, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkDescriptorSetLayoutCreateInfo(const VkDescriptorSetLayoutCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkDescriptorSetLayoutCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_pnext(value->pNext, stream, packed.offset + offsetof(VkDescriptorSetLayoutCreateInfo, pNext), packed_value->pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_plain_array(value->pBindings, value->bindingCount, stream, packed.offset + offsetof(VkDescriptorSetLayoutCreateInfo, pBindings),
                          packed_value->pBindings);
}}

VkResult unpack_VkDescriptorSetLayoutCreateInfo(SafeArrayView<std::uint8_t>& view, const VkDescriptorSetLayoutCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkDescriptorSetLayoutCreateInfo) ? nullptr : reinterpret_cast<VkDescriptorSetLayoutCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pBindings, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkPipelineLayoutCreateInfo(const VkPipelineLayoutCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkPipelineLayoutCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_pnext(value->pNext, stream, packed.offset + offsetof(VkPipelineLayoutCreateInfo, pNext), packed_value->pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(value->pSetLayouts, value->setLayoutCount, stream, packed.offset + offsetof(VkPipelineLayoutCreateInfo, pSetLayouts),
                            packed_value->pSetLayouts);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_plain_array(value->pPushConstantRanges, value->pushConstantRangeCount, stream,
                          packed.offset + offsetof(VkPipelineLayoutCreateInfo, pPushConstantRanges), packed_value->pPushConstantRanges);
}}

VkResult unpack_VkPipelineLayoutCreateInfo(SafeArrayView<std::uint8_t>& view, const VkPipelineLayoutCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkPipelineLayoutCreateInfo) ? nullptr : reinterpret_cast<VkPipelineLayoutCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pSetLayouts, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pPushConstantRanges, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkRenderPassCreateInfo(const VkRenderPassCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkRenderPassCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  status = pack_pnext(value->pNext, stream, packed.offset + offsetof(VkRenderPassCreateInfo, pNext), packed_value->pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(value->pAttachments, value->attachmentCount, stream, packed.offset + offsetof(VkRenderPassCreateInfo, pAttachments),
                            packed_value->pAttachments);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_nested_struct_array(value->pSubpasses, value->subpassCount, stream, packed.offset + offsetof(VkRenderPassCreateInfo, pSubpasses),
                                    packed_value->pSubpasses,
                                    [](const VkSubpassDescription& source, VkSubpassDescription& packed, CommandStream& s, std::size_t item_offset) {{
                                      VkResult nested = pack_plain_array(source.pInputAttachments, source.inputAttachmentCount, s,
                                                                        item_offset + offsetof(VkSubpassDescription, pInputAttachments),
                                                                        packed.pInputAttachments);
                                      if (nested != VK_SUCCESS) {{ return nested; }}
                                      nested = pack_plain_array(source.pColorAttachments, source.colorAttachmentCount, s,
                                                                item_offset + offsetof(VkSubpassDescription, pColorAttachments), packed.pColorAttachments);
                                      if (nested != VK_SUCCESS) {{ return nested; }}
                                      nested = pack_plain_array(source.pResolveAttachments, source.colorAttachmentCount, s,
                                                                item_offset + offsetof(VkSubpassDescription, pResolveAttachments),
                                                                packed.pResolveAttachments);
                                      if (nested != VK_SUCCESS) {{ return nested; }}
                                      nested = pack_plain_array(source.pDepthStencilAttachment, source.pDepthStencilAttachment ? 1u : 0u, s,
                                                                item_offset + offsetof(VkSubpassDescription, pDepthStencilAttachment),
                                                                packed.pDepthStencilAttachment);
                                      if (nested != VK_SUCCESS) {{ return nested; }}
                                      return pack_plain_array(source.pPreserveAttachments, source.preserveAttachmentCount, s,
                                                              item_offset + offsetof(VkSubpassDescription, pPreserveAttachments),
                                                              packed.pPreserveAttachments);
                                    }});
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  return pack_plain_array(value->pDependencies, value->dependencyCount, stream, packed.offset + offsetof(VkRenderPassCreateInfo, pDependencies),
                          packed_value->pDependencies);
}}

VkResult unpack_VkRenderPassCreateInfo(SafeArrayView<std::uint8_t>& view, const VkRenderPassCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkRenderPassCreateInfo) ? nullptr : reinterpret_cast<VkRenderPassCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pAttachments, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed->pSubpasses, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  for (std::uint32_t i = 0; typed->pSubpasses && i < typed->subpassCount; ++i) {{
    auto& subpass = const_cast<VkSubpassDescription&>(typed->pSubpasses[i]);
    status = recover_pointer(subpass.pInputAttachments, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(subpass.pColorAttachments, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(subpass.pResolveAttachments, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(subpass.pDepthStencilAttachment, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(subpass.pPreserveAttachments, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = recover_pointer(typed->pDependencies, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

namespace {{

VkResult pack_VkGraphicsPipelineCreateInfo_payload(const VkGraphicsPipelineCreateInfo& source, VkGraphicsPipelineCreateInfo& packed,
                                                   CommandStream& stream, std::size_t base_offset) {{
  VkResult status = pack_pnext(source.pNext, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pNext), packed.pNext);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_nested_struct_array(source.pStages, source.stageCount, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pStages),
                                    packed.pStages, pack_VkPipelineShaderStageCreateInfo_payload);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  std::size_t vertex_input_offset = 0;
  VkPipelineVertexInputStateCreateInfo* packed_vertex_input = nullptr;
  status = pack_plain_object(source.pVertexInputState, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pVertexInputState),
                             packed.pVertexInputState, vertex_input_offset, packed_vertex_input);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (source.pVertexInputState && packed_vertex_input) {{
    auto& vi = *packed_vertex_input;
    status = pack_plain_array(source.pVertexInputState->pVertexBindingDescriptions, source.pVertexInputState->vertexBindingDescriptionCount, stream,
                              vertex_input_offset + offsetof(VkPipelineVertexInputStateCreateInfo, pVertexBindingDescriptions),
                              vi.pVertexBindingDescriptions);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = pack_plain_array(source.pVertexInputState->pVertexAttributeDescriptions, source.pVertexInputState->vertexAttributeDescriptionCount, stream,
                              vertex_input_offset + offsetof(VkPipelineVertexInputStateCreateInfo, pVertexAttributeDescriptions),
                              vi.pVertexAttributeDescriptions);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = pack_plain_array(source.pInputAssemblyState, source.pInputAssemblyState ? 1u : 0u, stream,
                            base_offset + offsetof(VkGraphicsPipelineCreateInfo, pInputAssemblyState), packed.pInputAssemblyState);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(source.pTessellationState, source.pTessellationState ? 1u : 0u, stream,
                            base_offset + offsetof(VkGraphicsPipelineCreateInfo, pTessellationState), packed.pTessellationState);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  std::size_t viewport_offset = 0;
  VkPipelineViewportStateCreateInfo* packed_viewport = nullptr;
  status = pack_plain_object(source.pViewportState, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pViewportState),
                             packed.pViewportState, viewport_offset, packed_viewport);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (source.pViewportState && packed_viewport) {{
    auto& vp = *packed_viewport;
    status = pack_plain_array(source.pViewportState->pViewports, source.pViewportState->viewportCount, stream,
                              viewport_offset + offsetof(VkPipelineViewportStateCreateInfo, pViewports),
                              vp.pViewports);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = pack_plain_array(source.pViewportState->pScissors, source.pViewportState->scissorCount, stream,
                              viewport_offset + offsetof(VkPipelineViewportStateCreateInfo, pScissors),
                              vp.pScissors);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = pack_plain_array(source.pRasterizationState, source.pRasterizationState ? 1u : 0u, stream,
                            base_offset + offsetof(VkGraphicsPipelineCreateInfo, pRasterizationState), packed.pRasterizationState);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(source.pMultisampleState, source.pMultisampleState ? 1u : 0u, stream,
                            base_offset + offsetof(VkGraphicsPipelineCreateInfo, pMultisampleState), packed.pMultisampleState);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = pack_plain_array(source.pDepthStencilState, source.pDepthStencilState ? 1u : 0u, stream,
                            base_offset + offsetof(VkGraphicsPipelineCreateInfo, pDepthStencilState), packed.pDepthStencilState);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  std::size_t color_blend_offset = 0;
  VkPipelineColorBlendStateCreateInfo* packed_color_blend = nullptr;
  status = pack_plain_object(source.pColorBlendState, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pColorBlendState),
                             packed.pColorBlendState, color_blend_offset, packed_color_blend);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (source.pColorBlendState && packed_color_blend) {{
    auto& blend = *packed_color_blend;
    status = pack_plain_array(source.pColorBlendState->pAttachments, source.pColorBlendState->attachmentCount, stream,
                              color_blend_offset + offsetof(VkPipelineColorBlendStateCreateInfo, pAttachments),
                              blend.pAttachments);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  std::size_t dynamic_offset = 0;
  VkPipelineDynamicStateCreateInfo* packed_dynamic = nullptr;
  status = pack_plain_object(source.pDynamicState, stream, base_offset + offsetof(VkGraphicsPipelineCreateInfo, pDynamicState),
                             packed.pDynamicState, dynamic_offset, packed_dynamic);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (source.pDynamicState && packed_dynamic) {{
    auto& dynamic = *packed_dynamic;
    status = pack_plain_array(source.pDynamicState->pDynamicStates, source.pDynamicState->dynamicStateCount, stream,
                              dynamic_offset + offsetof(VkPipelineDynamicStateCreateInfo, pDynamicStates),
                              dynamic.pDynamicStates);
  }}
  return status;
}}

VkResult unpack_VkGraphicsPipelineCreateInfo_payload(SafeArrayView<std::uint8_t>& view, VkGraphicsPipelineCreateInfo& typed) {{
  VkResult status = recover_pnext(typed.pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pStages, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  for (std::uint32_t i = 0; typed.pStages && i < typed.stageCount; ++i) {{
    status = unpack_VkPipelineShaderStageCreateInfo_payload(view, const_cast<VkPipelineShaderStageCreateInfo&>(typed.pStages[i]));
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = recover_pointer(typed.pVertexInputState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (typed.pVertexInputState) {{
    auto& vi = const_cast<VkPipelineVertexInputStateCreateInfo&>(*typed.pVertexInputState);
    status = recover_pointer(vi.pVertexBindingDescriptions, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(vi.pVertexAttributeDescriptions, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = recover_pointer(typed.pInputAssemblyState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pTessellationState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pViewportState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (typed.pViewportState) {{
    auto& vp = const_cast<VkPipelineViewportStateCreateInfo&>(*typed.pViewportState);
    status = recover_pointer(vp.pViewports, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
    status = recover_pointer(vp.pScissors, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = recover_pointer(typed.pRasterizationState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pMultisampleState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pDepthStencilState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  status = recover_pointer(typed.pColorBlendState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (typed.pColorBlendState) {{
    auto& blend = const_cast<VkPipelineColorBlendStateCreateInfo&>(*typed.pColorBlendState);
    status = recover_pointer(blend.pAttachments, view);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  }}
  status = recover_pointer(typed.pDynamicState, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  if (typed.pDynamicState) {{
    auto& dynamic = const_cast<VkPipelineDynamicStateCreateInfo&>(*typed.pDynamicState);
    status = recover_pointer(dynamic.pDynamicStates, view);
  }}
  return status;
}}

}} // namespace

VkResult pack_VkGraphicsPipelineCreateInfo(const VkGraphicsPipelineCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkGraphicsPipelineCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  return pack_VkGraphicsPipelineCreateInfo_payload(*value, *packed_value, stream, packed.offset);
}}

VkResult unpack_VkGraphicsPipelineCreateInfo(SafeArrayView<std::uint8_t>& view, const VkGraphicsPipelineCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkGraphicsPipelineCreateInfo) ? nullptr : reinterpret_cast<VkGraphicsPipelineCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = unpack_VkGraphicsPipelineCreateInfo_payload(view, *typed);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

VkResult pack_VkSemaphoreCreateInfo(const VkSemaphoreCreateInfo* value, CommandStream& stream, PackedStruct& packed) {{
  VkSemaphoreCreateInfo* packed_value = nullptr;
  VkResult status = append_shallow_struct(value, stream, packed, packed_value);
  if (status != VK_SUCCESS || !value) {{ return status; }}
  return pack_pnext(value->pNext, stream, packed.offset + offsetof(VkSemaphoreCreateInfo, pNext), packed_value->pNext);
}}

VkResult unpack_VkSemaphoreCreateInfo(SafeArrayView<std::uint8_t>& view, const VkSemaphoreCreateInfo** value) {{
  auto* typed = view.size() < sizeof(VkSemaphoreCreateInfo) ? nullptr : reinterpret_cast<VkSemaphoreCreateInfo*>(view.address(0));
  if (!typed || typed->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
  VkResult status = recover_pnext(typed->pNext, view);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  *value = typed;
  return VK_SUCCESS;
}}

#define VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(Type)                                                                                                      \\
  VkResult pack_array_##Type(const Type* values, std::uint32_t count, CommandStream& stream, PackedStruct& packed) {{                                \\
    packed.offset = 0;                                                                                                                               \\
    if (!values || count == 0) {{ return VK_SUCCESS; }}                                                                                              \\
    const Type* pointer_slot = nullptr;                                                                                                              \\
    return pack_nested_struct_array(values, count, stream, 0, pointer_slot,                                                                          \\
                                    [](const Type& source, Type& packed_value, CommandStream& nested_stream, std::size_t item_offset) {{             \\
                                      PackedStruct unused {{.offset = item_offset}};                                                                  \\
                                      (void)unused;                                                                                                  \\
                                      return pack_##Type(&source, nested_stream, unused);                                                            \\
                                    });                                                                                                              \\
  }                                                                                                                                                  \\
  VkResult unpack_array_##Type(SafeArrayView<std::uint8_t>& view, std::uint32_t count, const Type** values) {{                                       \\
    if (!values) {{ return VK_ERROR_UNKNOWN; }}                                                                                                      \\
    *values = view.size() < sizeof(Type) * count ? nullptr : reinterpret_cast<Type*>(view.address(0));                                                \\
    if (!*values && count != 0) {{ return VK_ERROR_UNKNOWN; }}                                                                                       \\
    for (std::uint32_t i = 0; i < count; ++i) {{                                                                                                     \\
      auto child = tail_view_from_pointer(view, &(*values)[i]);                                                                                      \\
      const Type* ignored = nullptr;                                                                                                                 \\
      VkResult status = unpack_##Type(child, &ignored);                                                                                              \\
      if (status != VK_SUCCESS) {{ return status; }}                                                                                                 \\
    }}                                                                                                                                               \\
    return VK_SUCCESS;                                                                                                                               \\
  }}

VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkBufferCreateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkMemoryAllocateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkShaderModuleCreateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkDescriptorSetLayoutCreateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkPipelineLayoutCreateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkRenderPassCreateInfo)
VKFWD_DEFINE_COMMAND_STRUCT_ARRAY(VkSemaphoreCreateInfo)

VkResult pack_array_VkGraphicsPipelineCreateInfo(const VkGraphicsPipelineCreateInfo* values, std::uint32_t count, CommandStream& stream, PackedStruct& packed) {{
  packed.offset = 0;
  if (!values || count == 0) {{ return VK_SUCCESS; }}
  VkGraphicsPipelineCreateInfo* unused = nullptr;
  try {{
    std::size_t target = 0;
    auto destination = stream.grow<VkGraphicsPipelineCreateInfo>(count, alignof(VkGraphicsPipelineCreateInfo), &target);
    if (destination.set(0, count, values) != count) {{ return VK_ERROR_UNKNOWN; }}
    packed.offset = target;
    for (std::uint32_t i = 0; i < count; ++i) {{
      VkResult status = pack_VkGraphicsPipelineCreateInfo_payload(values[i], destination.at(i), stream, target + i * sizeof(VkGraphicsPipelineCreateInfo));
      if (status != VK_SUCCESS) {{ return status; }}
    }}
    (void)unused;
    return VK_SUCCESS;
  }} catch (const std::bad_alloc&) {{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

VkResult unpack_array_VkGraphicsPipelineCreateInfo(SafeArrayView<std::uint8_t>& view, std::uint32_t count, const VkGraphicsPipelineCreateInfo** values) {{
  if (!values) {{ return VK_ERROR_UNKNOWN; }}
  *values = view.size() < sizeof(VkGraphicsPipelineCreateInfo) * count ? nullptr : reinterpret_cast<VkGraphicsPipelineCreateInfo*>(view.address(0));
  if (!*values && count != 0) {{ return VK_ERROR_UNKNOWN; }}
  for (std::uint32_t i = 0; i < count; ++i) {{
    auto& item = const_cast<VkGraphicsPipelineCreateInfo&>((*values)[i]);
    if (item.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {{ return VK_ERROR_UNKNOWN; }}
    VkResult status = unpack_VkGraphicsPipelineCreateInfo_payload(view, item);
    if (status != VK_SUCCESS) {{ return status; }}
  }}
  return VK_SUCCESS;
}}

}} // namespace vkfwd::generated::structure
"""
    return (
        content.replace(
            "@VULKAN_API_VERSION@", str(metadata["versions"]["vulkan_api_version"])
        )
        .replace("@VK_XML_SHA256@", str(metadata["generator"]["vk_xml_sha256"]))
        .replace("{{", "{")
        .replace("}}", "}")
    )


def write_command_structure_files(
    metadata: dict[str, object], output_dir: Path
) -> None:
    structure_dir = output_dir / "structure"
    structure_dir.mkdir(parents=True, exist_ok=True)
    (structure_dir / "command_structs.hpp").write_text(
        command_structs_header_content(metadata), encoding="utf-8"
    )
    (structure_dir / "command_structs.cpp").write_text(
        command_structs_source_content(metadata), encoding="utf-8"
    )


def command_test_sample_expression(
    command: dict[str, object], parameter: dict[str, object]
) -> str:
    name = str(parameter["name"])
    ptype = str(parameter["type"])
    if str(command["name"]) == "vkMapMemory" and name == "ppData":
        # A mapped-memory address is only meaningful in the process that called
        # vkMapMemory. Returning a receiver-side pointer to the source
        # application would violate both address-space and lifetime ownership;
        # keep this null until an explicit mapped-memory proxy protocol exists.
        return "nullptr"
    scalar_by_name = {
        "pApiVersion": "&samples.api_version",
        "pPropertyCount": "&samples.count",
        "pPhysicalDeviceCount": "&samples.count",
        "pQueueFamilyPropertyCount": "&samples.count",
        "createInfoCount": "static_cast<std::uint32_t>(samples.vk_graphics_pipeline_create_info_array.size())",
        "queueFamilyIndex": "samples.queue_family_index",
        "queueIndex": "samples.queue_index",
        "memoryOffset": "samples.memory_offset",
        "offset": "samples.memory_offset",
        "size": "samples.memory_size",
        "flags": f"{ptype} {{0}}",
    }
    if name in scalar_by_name:
        return scalar_by_name[name]
    if name == "pAllocator":
        return "&samples.allocator"
    if ptype == "char":
        return "samples.layer_name"
    if ptype in SUPPORTED_COMMAND_STRUCT_PARAMETERS:
        if bool(parameter.get("len")):
            return f"samples.{command_field_name(ptype)}_array.data()"
        return f"&samples.{command_field_name(ptype)}"
    pointer_depth_value = int(parameter["pointer_depth"])
    if parameter["direction"] == "output":
        if pointer_depth_value == 2 and ptype == "void":
            return "&samples.mapped_data"
        if parameter_is_output_array(parameter):
            return f"samples.{command_field_name(ptype)}_array.data()"
        return f"&samples.{command_field_name(ptype)}"
    if pointer_depth_value == 1:
        return f"&samples.{command_field_name(ptype)}"
    if ptype in {
        "VkInstance",
        "VkPhysicalDevice",
        "VkDevice",
        "VkQueue",
        "VkBuffer",
        "VkDeviceMemory",
        "VkShaderModule",
        "VkDescriptorSetLayout",
        "VkPipelineLayout",
        "VkRenderPass",
        "VkPipeline",
        "VkSemaphore",
        "VkPipelineCache",
    }:
        return f"samples.{command_field_name(ptype)}"
    if ptype == "uint32_t":
        return "samples.count"
    if ptype == "VkDeviceSize":
        return "samples.memory_size"
    return f"{ptype} {{}}"


def command_test_response_initializer_fields(command: dict[str, object]) -> str:
    fields = []
    if str(command["return_type"]) != "void":
        fields.append(".return_value = VK_SUCCESS")
    output_array_counts = {
        str(parameter.get("len")).split(",", 1)[0]
        for parameter in command["parameters"]
        if parameter_is_output_array(parameter) and parameter.get("len")
    }
    output_names = {
        str(parameter["name"])
        for parameter in command["parameters"]
        if parameter["direction"] == "output"
    }
    for parameter in command["parameters"]:
        if parameter["direction"] == "output":
            fields.append(
                f".{parameter['name']} = {command_test_sample_expression(command, parameter)}"
            )
    for parameter in command["parameters"]:
        name = str(parameter["name"])
        if name in output_array_counts and name not in output_names:
            fields.append(
                f".{name} = {command_test_sample_expression(command, parameter)}"
            )
    return ",\n        ".join(fields)


def command_test_parameter_assertion(
    command: dict[str, object],
    parameter: dict[str, object],
    object_name: str,
    view_name: str,
) -> str:
    name = str(parameter["name"])
    ptype = str(parameter["type"])
    pointer_depth_value = int(parameter["pointer_depth"])
    if str(command["name"]) == "vkMapMemory" and name == "ppData":
        # The pack/unpack foundation test documents the current forwarding
        # boundary: vkMapMemory may serialize the call, but the caller-visible
        # mapped pointer requires a dedicated shared-memory/copy protocol.
        return f"    CHECK({object_name}->{name} == nullptr);"
    if name == "pAllocator":
        return f"    CHECK({object_name}->{name} == nullptr);"
    if pointer_depth_value == 0:
        return f"    CHECK({object_name}->{name} == {command_test_sample_expression(command, parameter)});"
    if ptype == "char":
        return f"    check_string({object_name}->{name}, samples.layer_name);"
    lines = [
        f"    REQUIRE({object_name}->{name} != nullptr);",
        f"    CHECK(points_into({view_name}, {object_name}->{name}));",
    ]
    if parameter_is_output_array(parameter):
        lines.append(
            f"    check_object({object_name}->{name}[0], samples.{command_field_name(ptype)}_array[0]);"
        )
    elif pointer_depth_value == 2 and ptype == "void":
        lines.append(f"    CHECK(*{object_name}->{name} == samples.mapped_data);")
    elif ptype in SUPPORTED_COMMAND_STRUCT_PARAMETERS:
        lines.append(
            f"    CHECK({object_name}->{name}->sType == samples.{command_field_name(ptype)}.sType);"
        )
    elif name in {"pApiVersion"}:
        lines.append(f"    CHECK(*{object_name}->{name} == samples.api_version);")
    elif name in {
        "pPropertyCount",
        "pPhysicalDeviceCount",
        "pQueueFamilyPropertyCount",
    }:
        lines.append(f"    CHECK(*{object_name}->{name} == samples.count);")
    else:
        lines.append(
            f"    check_object(*{object_name}->{name}, samples.{command_field_name(ptype)});"
        )
    return "\n".join(lines)


def command_test_response_assertion(
    command: dict[str, object],
    parameter: dict[str, object],
    object_name: str,
    view_name: str,
) -> str:
    return command_test_parameter_assertion(command, parameter, object_name, view_name)


def command_smoke_test_content(command: dict[str, object]) -> str:
    namespace = command_namespace(str(command["name"]))
    parameter_fields = ",\n        ".join(
        f".{parameter['name']} = {command_test_sample_expression(command, parameter)}"
        for parameter in command["parameters"]
    )
    parameter_assertions = "\n".join(
        command_test_parameter_assertion(
            command, parameter, "unpacked", "parameter_view"
        )
        for parameter in command["parameters"]
    )
    response_check = ""
    if command_needs_response(command):
        response_fields = command_test_response_initializer_fields(command)
        response_assertions = "\n".join(
            command_test_response_assertion(
                command, parameter, "unpacked_response", "response_view"
            )
            for parameter in command["parameters"]
            if parameter["direction"] == "output"
        )
        response_check = """
    CommandStream response_stream(64);
    typename Command::Response response {
        $response_fields
    };
    REQUIRE(Command::pack_response(response_stream, response) == VK_SUCCESS);
    CommandStream flattened_response = response_stream.flatten();
    auto response_view = full_view(flattened_response);
    const typename Command::Response * unpacked_response = nullptr;
    REQUIRE(Command::unpack_response(response_view, &unpacked_response) == VK_SUCCESS);
    REQUIRE(points_into(response_view, unpacked_response));
    $response_assertions
""".replace(
            "$response_fields", response_fields
        ).replace(
            "$response_assertions", response_assertions
        )
    return f"""
TEST_CASE("generated {command['name']} non-null parameter pack/unpack roundtrip") {{
    using Command = commands::{namespace}::Command;
    CommandSamples samples;
    CommandStream stream(64);
    typename Command::Parameters parameters {{
        {parameter_fields}
    }};
    REQUIRE(Command::pack_parameters(stream, parameters) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    auto parameter_view = full_view(flattened);
    const typename Command::Parameters * unpacked = nullptr;
    REQUIRE(Command::unpack_parameters(parameter_view, &unpacked) == VK_SUCCESS);
    REQUIRE(points_into(parameter_view, unpacked));
{parameter_assertions}
{response_check}}}
"""


def command_roundtrip_test_content(metadata: dict[str, object]) -> str:
    command_includes = "\n".join(
        f'#include "generated/command/{command["name"]}.hpp"'
        for command in metadata["commands"]
    )
    smoke_tests = "\n".join(
        command_smoke_test_content(command) for command in metadata["commands"]
    )
    return f"""#include "command_stream.hpp"
{command_includes}
#include "generated/structure/core.hpp"
#include "generated/structure/command_structs.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace vkfwd::generated::commands::test {{
namespace {{

template<class Pointer>
bool points_into(SafeArrayView<std::uint8_t> & view, Pointer pointer) {{
    auto * begin = view.address(0);
    if (!begin || !pointer) {{ return false; }}
    const auto * target = reinterpret_cast<const std::uint8_t *>(pointer);
    return target >= begin && target < begin + view.size();
}}

SafeArrayView<std::uint8_t> full_view(CommandStream & stream) {{
    return stream.at(0, stream.size());
}}

SafeArrayView<std::uint8_t> tail_view(CommandStream & stream, std::size_t offset) {{
    return stream.at(offset, stream.size() - offset);
}}

template<class T>
constexpr std::size_t command_payload_offset() {{
    constexpr std::size_t alignment = alignof(T);
    return (sizeof(CommandChunkHeader) + alignment - 1) & ~(alignment - 1);
}}

template<class T>
const T * packed_command_payload(CommandStream & stream) {{
    const auto view = stream.at(command_payload_offset<T>(), sizeof(T));
    REQUIRE(!view.empty());
    return reinterpret_cast<const T *>(view.address(0));
}}

void check_string(const char * actual, std::string_view expected) {{
    REQUIRE(actual != nullptr);
    CHECK(std::string_view(actual, expected.size()) == expected);
    CHECK(actual[expected.size()] == '\\0');
}}

template<class T>
void check_object(const T & actual, const T & expected) {{
    // Command pack/unpack tests care about byte-for-byte payload ownership, not
    // semantic Vulkan equality. Most Vulkan structs deliberately do not define
    // operator==, and copied handles are opaque values at this layer.
    CHECK(std::memcmp(&actual, &expected, sizeof(T)) == 0);
}}

template<class T>
void check_numeric_array(const T * actual, std::initializer_list<T> expected) {{
    REQUIRE(actual != nullptr);
    std::size_t index = 0;
    for (const T & value : expected) {{
        CHECK(actual[index] == value);
        ++index;
    }}
}}

inline void * VKAPI_PTR test_allocation(void *, std::size_t, std::size_t, VkSystemAllocationScope) {{ return nullptr; }}
inline void * VKAPI_PTR test_reallocation(void *, void *, std::size_t, std::size_t, VkSystemAllocationScope) {{ return nullptr; }}
inline void VKAPI_PTR test_free(void *, void *) {{}}
inline void VKAPI_PTR test_internal_allocation(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {{}}
inline void VKAPI_PTR test_internal_free(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {{}}

VkAllocationCallbacks test_allocator(void * user_data) {{
    return VkAllocationCallbacks {{
        .pUserData            = user_data,
        .pfnAllocation        = test_allocation,
        .pfnReallocation      = test_reallocation,
        .pfnFree              = test_free,
        .pfnInternalAllocation = test_internal_allocation,
        .pfnInternalFree       = test_internal_free,
    }};
}}

template<class Handle>
Handle test_handle(std::uintptr_t value) {{
    return reinterpret_cast<Handle>(value);
}}

VkApplicationInfo make_application_info() {{
    return VkApplicationInfo {{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "vkfwd-roundtrip-app",
        .applicationVersion = 13,
        .pEngineName        = "vkfwd-roundtrip-engine",
        .engineVersion      = 17,
        .apiVersion         = VK_MAKE_API_VERSION(0, 1, 4, 0),
    }};
}}

VkDeviceQueueCreateInfo make_queue_info(const float * priorities, std::uint32_t count) {{
    return VkDeviceQueueCreateInfo {{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VkDeviceQueueCreateFlags {{0x2}},
        .queueFamilyIndex = 5,
        .queueCount       = count,
        .pQueuePriorities = priorities,
    }};
}}

struct CommandSamples {{
    int allocator_user_data = 0x51;
    VkAllocationCallbacks allocator = test_allocator(&allocator_user_data);
    const char * layer_name = "VK_LAYER_VKFWD_sample";
    const char * extension_name = "VK_EXT_vkfwd_sample";
    std::uint32_t api_version = VK_MAKE_API_VERSION(0, 1, 4, 0);
    std::uint32_t count = 2;
    std::uint32_t queue_family_index = 1;
    std::uint32_t queue_index = 0;
    VkDeviceSize memory_offset = 16;
    VkDeviceSize memory_size = 256;

    VkInstance vk_instance = test_handle<VkInstance>(0x101);
    VkPhysicalDevice vk_physical_device = test_handle<VkPhysicalDevice>(0x102);
    VkDevice vk_device = test_handle<VkDevice>(0x103);
    VkQueue vk_queue = test_handle<VkQueue>(0x104);
    VkBuffer vk_buffer = test_handle<VkBuffer>(0x105);
    VkDeviceMemory vk_device_memory = test_handle<VkDeviceMemory>(0x106);
    VkShaderModule vk_shader_module = test_handle<VkShaderModule>(0x107);
    VkDescriptorSetLayout vk_descriptor_set_layout = test_handle<VkDescriptorSetLayout>(0x108);
    VkPipelineLayout vk_pipeline_layout = test_handle<VkPipelineLayout>(0x109);
    VkRenderPass vk_render_pass = test_handle<VkRenderPass>(0x10a);
    VkPipeline vk_pipeline = test_handle<VkPipeline>(0x10b);
    VkSemaphore vk_semaphore = test_handle<VkSemaphore>(0x10c);
    VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;
    void * mapped_data = reinterpret_cast<void *>(0x10d);

    VkPhysicalDeviceProperties vk_physical_device_properties {{}};
    VkPhysicalDeviceFeatures vk_physical_device_features {{}};
    VkPhysicalDeviceMemoryProperties vk_physical_device_memory_properties {{}};
    VkMemoryRequirements vk_memory_requirements {{.size = 4096, .alignment = 256, .memoryTypeBits = 0x3}};

    VkApplicationInfo vk_application_info = make_application_info();
    std::array<const char *, 1> enabled_layers {{{{layer_name}}}};
    std::array<const char *, 1> enabled_extensions {{{{extension_name}}}};
    std::array<float, 2> queue_priorities {{{{0.25f, 0.75f}}}};
    VkDeviceQueueCreateInfo vk_device_queue_create_info = make_queue_info(queue_priorities.data(), static_cast<std::uint32_t>(queue_priorities.size()));
    VkInstanceCreateInfo vk_instance_create_info {{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_application_info,
        .enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size()),
        .ppEnabledLayerNames = enabled_layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
    }};
    VkDeviceCreateInfo vk_device_create_info {{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &vk_device_queue_create_info,
        .enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size()),
        .ppEnabledLayerNames = enabled_layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
    }};

    std::array<VkLayerProperties, 2> vk_layer_properties_array {{{{
        VkLayerProperties {{.layerName = "VK_LAYER_VKFWD_a", .specVersion = 1, .implementationVersion = 2}},
        VkLayerProperties {{.layerName = "VK_LAYER_VKFWD_b", .specVersion = 3, .implementationVersion = 4}},
    }}}};
    std::array<VkExtensionProperties, 2> vk_extension_properties_array {{{{
        VkExtensionProperties {{.extensionName = "VK_EXT_vkfwd_a", .specVersion = 1}},
        VkExtensionProperties {{.extensionName = "VK_EXT_vkfwd_b", .specVersion = 2}},
    }}}};
    std::array<VkPhysicalDevice, 2> vk_physical_device_array {{{{
        test_handle<VkPhysicalDevice>(0x201),
        test_handle<VkPhysicalDevice>(0x202),
    }}}};
    std::array<VkQueueFamilyProperties, 2> vk_queue_family_properties_array {{{{
        VkQueueFamilyProperties {{.queueFlags = VK_QUEUE_GRAPHICS_BIT, .queueCount = 1}},
        VkQueueFamilyProperties {{.queueFlags = VK_QUEUE_TRANSFER_BIT, .queueCount = 1}},
    }}}};
    std::array<VkPipeline, 1> vk_pipeline_array {{{{
        test_handle<VkPipeline>(0x305),
    }}}};

    std::array<std::uint32_t, 2> queue_family_indices {{1, 2}};
    std::array<std::uint32_t, 4> shader_code {{0x07230203u, 0x00010000u, 0x0008000au, 0u}};
    std::array<VkDescriptorSetLayoutBinding, 1> descriptor_bindings {{{{
        VkDescriptorSetLayoutBinding {{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}},
    }}}};
    std::array<VkDescriptorSetLayout, 1> descriptor_set_layouts {{vk_descriptor_set_layout}};
    std::array<VkPushConstantRange, 1> push_constant_ranges {{{{
        VkPushConstantRange {{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 16}},
    }}}};
    std::array<VkAttachmentDescription, 1> attachments {{{{
        VkAttachmentDescription {{.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT}},
    }}}};
    std::array<VkAttachmentReference, 1> color_attachments {{{{
        VkAttachmentReference {{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},
    }}}};
    std::array<VkSubpassDescription, 1> subpasses {{{{
        VkSubpassDescription {{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = color_attachments.data()}},
    }}}};
    VkPipelineShaderStageCreateInfo pipeline_stage {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vk_shader_module,
        .pName = "main",
    }};
    std::array<VkVertexInputBindingDescription, 1> vertex_bindings {{{{
        VkVertexInputBindingDescription {{.binding = 0, .stride = 8}},
    }}}};
    std::array<VkVertexInputAttributeDescription, 1> vertex_attributes {{{{
        VkVertexInputAttributeDescription {{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT}},
    }}}};
    VkPipelineVertexInputStateCreateInfo vertex_input {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = static_cast<std::uint32_t>(vertex_bindings.size()),
        .pVertexBindingDescriptions = vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    }};
    VkPipelineInputAssemblyStateCreateInfo input_assembly {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    }};
    std::array<VkDynamicState, 1> dynamic_states {{VK_DYNAMIC_STATE_VIEWPORT}};
    VkPipelineDynamicStateCreateInfo dynamic_state {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    }};

    VkBufferCreateInfo vk_buffer_create_info {{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 1024,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = static_cast<std::uint32_t>(queue_family_indices.size()),
        .pQueueFamilyIndices = queue_family_indices.data(),
    }};
    VkMemoryAllocateInfo vk_memory_allocate_info {{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 2048,
        .memoryTypeIndex = 1,
    }};
    VkShaderModuleCreateInfo vk_shader_module_create_info {{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_code.size() * sizeof(std::uint32_t),
        .pCode = shader_code.data(),
    }};
    VkDescriptorSetLayoutCreateInfo vk_descriptor_set_layout_create_info {{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(descriptor_bindings.size()),
        .pBindings = descriptor_bindings.data(),
    }};
    VkPipelineLayoutCreateInfo vk_pipeline_layout_create_info {{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(descriptor_set_layouts.size()),
        .pSetLayouts = descriptor_set_layouts.data(),
        .pushConstantRangeCount = static_cast<std::uint32_t>(push_constant_ranges.size()),
        .pPushConstantRanges = push_constant_ranges.data(),
    }};
    VkRenderPassCreateInfo vk_render_pass_create_info {{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = static_cast<std::uint32_t>(subpasses.size()),
        .pSubpasses = subpasses.data(),
    }};
    VkGraphicsPipelineCreateInfo vk_graphics_pipeline_create_info {{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1,
        .pStages = &pipeline_stage,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pDynamicState = &dynamic_state,
        .layout = vk_pipeline_layout,
        .renderPass = vk_render_pass,
    }};
    std::array<VkGraphicsPipelineCreateInfo, 1> vk_graphics_pipeline_create_info_array {{{{
        vk_graphics_pipeline_create_info,
    }}}};
    VkSemaphoreCreateInfo vk_semaphore_create_info {{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    }};
}};

}} // namespace

{smoke_tests}

TEST_CASE("generated vkCreateInstance parameter pack flatten unpack reconstructs every pointer into flattened stream") {{
    using Command = commands::vkCreateInstance::Command;
    CommandStream stream(64);
    int allocator_user_data = 0x31;
    auto allocator = test_allocator(&allocator_user_data);
    auto app = make_application_info();
    std::array<const char *, 1> layers {{"VK_LAYER_VKFWD_instance"}};
    std::array<const char *, 1> extensions {{"VK_KHR_surface"}};
    VkInstanceCreateInfo create_info {{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkInstanceCreateFlags {{0x4}},
        .pApplicationInfo        = &app,
        .enabledLayerCount       = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames     = layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    }};
    VkInstance source_instance = test_handle<VkInstance>(0x404);
    Command::Parameters parameters {{
        .pCreateInfo = &create_info,
        .pAllocator  = &allocator,
        .pInstance   = &source_instance,
    }};

    REQUIRE(Command::pack_parameters(stream, parameters) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    const auto * packed_parameters = packed_command_payload<Command::Parameters>(flattened);
    CHECK(packed_parameters->pAllocator == nullptr);
    auto view = full_view(flattened);
    const Command::Parameters * actual = nullptr;
    REQUIRE(Command::unpack_parameters(view, &actual) == VK_SUCCESS);

    REQUIRE(points_into(view, actual));
    REQUIRE(points_into(view, actual->pCreateInfo));
    CHECK(actual->pAllocator == nullptr);
    REQUIRE(points_into(view, actual->pInstance));
    CHECK(actual->pInstance != &source_instance);
    CHECK(*actual->pInstance == source_instance);
    check_string(actual->pCreateInfo->pApplicationInfo->pApplicationName, app.pApplicationName);
    check_string(actual->pCreateInfo->ppEnabledLayerNames[0], layers[0]);
    check_string(actual->pCreateInfo->ppEnabledExtensionNames[0], extensions[0]);
}}

TEST_CASE("generated vkCreateDevice parameter pack flatten unpack reconstructs every pointer into flattened stream") {{
    using Command = commands::vkCreateDevice::Command;
    CommandStream stream(64);
    int allocator_user_data = 0x42;
    auto allocator = test_allocator(&allocator_user_data);
    std::array<float, 2> priorities {{0.25f, 0.75f}};
    auto queue = make_queue_info(priorities.data(), static_cast<std::uint32_t>(priorities.size()));
    std::array<const char *, 1> layers {{"VK_LAYER_VKFWD_device"}};
    std::array<const char *, 1> extensions {{"VK_KHR_swapchain"}};
    VkPhysicalDeviceFeatures features {{}};
    features.robustBufferAccess = VK_TRUE;
    VkDeviceCreateInfo create_info {{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkDeviceCreateFlags {{0x8}},
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queue,
        .enabledLayerCount       = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames     = layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
        .pEnabledFeatures        = &features,
    }};
    VkDevice source_device = test_handle<VkDevice>(0x505);
    Command::Parameters parameters {{
        .physicalDevice = test_handle<VkPhysicalDevice>(0x303),
        .pCreateInfo    = &create_info,
        .pAllocator     = &allocator,
        .pDevice        = &source_device,
    }};

    REQUIRE(Command::pack_parameters(stream, parameters) == VK_SUCCESS);
    CommandStream flattened = stream.flatten();
    const auto * packed_parameters = packed_command_payload<Command::Parameters>(flattened);
    CHECK(packed_parameters->pAllocator == nullptr);
    auto view = full_view(flattened);
    const Command::Parameters * actual = nullptr;
    REQUIRE(Command::unpack_parameters(view, &actual) == VK_SUCCESS);

    REQUIRE(points_into(view, actual));
    REQUIRE(points_into(view, actual->pCreateInfo));
    CHECK(actual->pAllocator == nullptr);
    REQUIRE(points_into(view, actual->pDevice));
    CHECK(actual->pDevice != &source_device);
    CHECK(*actual->pDevice == source_device);
    check_numeric_array(actual->pCreateInfo->pQueueCreateInfos->pQueuePriorities, {{0.25f, 0.75f}});
    CHECK(actual->pCreateInfo->pEnabledFeatures->robustBufferAccess == VK_TRUE);
}}

TEST_CASE("generated destroy command parameter pack flatten unpack drops allocator callbacks") {{
    int allocator_user_data = 0x51;
    auto allocator = test_allocator(&allocator_user_data);

    {{
        using Command = commands::vkDestroyInstance::Command;
        CommandStream stream(64);
        Command::Parameters parameters {{
            .instance   = test_handle<VkInstance>(0x601),
            .pAllocator = &allocator,
        }};
        REQUIRE(Command::pack_parameters(stream, parameters) == VK_SUCCESS);
        CommandStream flattened = stream.flatten();
        const auto * packed_parameters = packed_command_payload<Command::Parameters>(flattened);
        CHECK(packed_parameters->pAllocator == nullptr);
        auto view = full_view(flattened);
        const Command::Parameters * actual = nullptr;
        REQUIRE(Command::unpack_parameters(view, &actual) == VK_SUCCESS);
        REQUIRE(points_into(view, actual));
        CHECK(actual->pAllocator == nullptr);
        CHECK(actual->instance == parameters.instance);
    }}

    {{
        using Command = commands::vkDestroyDevice::Command;
        CommandStream stream(64);
        Command::Parameters parameters {{
            .device     = test_handle<VkDevice>(0x602),
            .pAllocator = &allocator,
        }};
        REQUIRE(Command::pack_parameters(stream, parameters) == VK_SUCCESS);
        CommandStream flattened = stream.flatten();
        const auto * packed_parameters = packed_command_payload<Command::Parameters>(flattened);
        CHECK(packed_parameters->pAllocator == nullptr);
        auto view = full_view(flattened);
        const Command::Parameters * actual = nullptr;
        REQUIRE(Command::unpack_parameters(view, &actual) == VK_SUCCESS);
        REQUIRE(points_into(view, actual));
        CHECK(actual->pAllocator == nullptr);
        CHECK(actual->device == parameters.device);
    }}
}}

TEST_CASE("generated create command responses pack flatten unpack reconstruct output pointers into flattened stream") {{
    {{
        using Command = commands::vkCreateInstance::Command;
        CommandStream stream(64);
        VkInstance instance = test_handle<VkInstance>(0x701);
        Command::Response response {{
            .return_value = VK_SUCCESS,
            .pInstance    = &instance,
        }};
        REQUIRE(Command::pack_response(stream, response) == VK_SUCCESS);
        CommandStream flattened = stream.flatten();
        auto view = full_view(flattened);
        const Command::Response * actual = nullptr;
        REQUIRE(Command::unpack_response(view, &actual) == VK_SUCCESS);
        REQUIRE(points_into(view, actual));
        REQUIRE(points_into(view, actual->pInstance));
        CHECK(*actual->pInstance == instance);
    }}

    {{
        using Command = commands::vkCreateDevice::Command;
        CommandStream stream(64);
        VkDevice device = test_handle<VkDevice>(0x702);
        Command::Response response {{
            .return_value = VK_SUCCESS,
            .pDevice      = &device,
        }};
        REQUIRE(Command::pack_response(stream, response) == VK_SUCCESS);
        CommandStream flattened = stream.flatten();
        auto view = full_view(flattened);
        const Command::Response * actual = nullptr;
        REQUIRE(Command::unpack_response(view, &actual) == VK_SUCCESS);
        REQUIRE(points_into(view, actual));
        REQUIRE(points_into(view, actual->pDevice));
        CHECK(*actual->pDevice == device);
    }}
}}

}} // namespace vkfwd::generated::commands::test
"""


def write_command_test_files(metadata: dict[str, object], output_dir: Path) -> None:
    test_dir = output_dir / "command" / "test"
    test_dir.mkdir(parents=True, exist_ok=True)
    (test_dir / "pack_unpack_roundtrip_test.cpp").write_text(
        command_roundtrip_test_content(metadata), encoding="utf-8"
    )
    (test_dir / "internal-test.cmake").write_text(
        """# This generated manifest is consumed by dev/test/internal-test/CMakeLists.txt.
# Keep generated command pack/flatten/unpack tests beside generated command serializers.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  pack_unpack_roundtrip_test.cpp)
""",
        encoding="utf-8",
    )


def format_generated_files(
    output_dir: Path, forwarder_output_dir: Path, receiver_output_dir: Path
) -> None:
    root_dir = repo_root()
    root_resolved = root_dir.resolve()
    scopes = []
    for output_path in (output_dir, forwarder_output_dir, receiver_output_dir):
        resolved = output_path.resolve()
        try:
            scopes.append(resolved.relative_to(root_resolved).as_posix())
        except ValueError:
            continue

    if not scopes:
        return

    formatter = root_dir / "dev/bin/format-all-sources.py"
    # The shared formatter intentionally operates on tracked files only. When
    # generation targets scratch directories, callers still get raw generator
    # output for comparison instead of silently formatting files Git cannot see.
    subprocess.run(
        [sys.executable, str(formatter), "-q", *scopes],
        cwd=root_dir,
        check=True,
    )


def generate(
    output_dir: Path, forwarder_output_dir: Path, receiver_output_dir: Path
) -> None:
    root_dir = repo_root()
    xml_path = root_dir / "src/third_party/vulkan/registry/vk.xml"
    version_path = root_dir / "src/third_party/vulkan/VERSION"
    xml_bytes = xml_path.read_bytes()
    root = ET.fromstring(xml_bytes)
    versions = parse_version_file(version_path)
    vulkan_api = parse_semver(versions.get("vulkan_api_version"))
    handles = collect_handles(root)
    selected_commands = [
        command_metadata(root, name, handles) for name in TARGET_COMMANDS
    ]
    check_command_id_collisions(selected_commands)
    command_structs = {
        str(parameter["type"])
        for command in selected_commands
        for parameter in command["parameters"]
    }
    selected_handle_types = {
        str(parameter["type"])
        for command in selected_commands
        for parameter in command["parameters"]
        if str(parameter["type"]) in handles
    } | {
        str(handle)
        for command in selected_commands
        for handle in command["creates_handles"]
    }
    metadata: dict[str, object] = {
        "schema": "vkfwd.vulkan-metadata.v1",
        "generator": {
            "version": GENERATOR_VERSION,
            "vk_xml": "src/third_party/vulkan/registry/vk.xml",
            "vk_xml_sha256": hashlib.sha256(xml_bytes).hexdigest(),
        },
        "protocol": {
            "schema_version": SCHEMA_VERSION,
        },
        "versions": {
            "vulkan_api_version": versions.get("vulkan_api_version"),
            "vulkan_api": vulkan_api,
            "header_version": versions.get("header_version"),
            "upstream_tag": versions.get("upstream_tag"),
            "upstream_commit": versions.get("upstream_commit"),
        },
        "commands": selected_commands,
        "handles": {name: handles[name] for name in sorted(selected_handle_types)},
        "structs": collect_structs(root, command_structs),
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    write_manifest(metadata, output_dir / "vulkan_manifest.json")
    write_coverage(metadata, output_dir / "vulkan_coverage.md")
    write_manual_hooks_header(metadata, output_dir / "vulkan_manual_hooks.hpp")
    write_command_files(metadata, output_dir)
    write_vulkan_api_header(metadata, output_dir / "vulkan_api.hpp")
    write_dispatch_table_files(metadata, output_dir)
    write_command_structure_files(metadata, output_dir)
    write_structure_test_files(metadata, output_dir)
    write_command_test_files(metadata, output_dir)
    write_forwarder_files(metadata, forwarder_output_dir)
    write_receiver_files(metadata, receiver_output_dir)
    format_generated_files(output_dir, forwarder_output_dir, receiver_output_dir)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=repo_root() / "src/vkfwd/ferry/core/generated",
        help="directory for generated metadata files",
    )
    parser.add_argument(
        "--forwarder-output-dir",
        type=Path,
        default=repo_root() / "src/vkfwd/ferry/forwarder/generated",
        help="directory for generated forwarder files",
    )
    parser.add_argument(
        "--receiver-output-dir",
        type=Path,
        default=repo_root() / "src/vkfwd/ferry/receiver/generated",
        help="directory for generated receiver files",
    )
    args = parser.parse_args()
    generate(args.output_dir, args.forwarder_output_dir, args.receiver_output_dir)


if __name__ == "__main__":
    main()
