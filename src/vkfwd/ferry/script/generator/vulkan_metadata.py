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
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkCreateDevice",
    "vkDestroyDevice",
)
GENERATOR_VERSION = "vkfwd-vulkan-metadata-0.1"
SCHEMA_VERSION = 1
COMMAND_REVISION = 1
COMMAND_ID_SALT = "vkfwd.vulkan.command-id.v1:"


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
    digest = hashlib.sha256((COMMAND_ID_SALT + name).encode("utf-8")).digest()
    value = int.from_bytes(digest[:4], byteorder="big")
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
    for parameter in command["parameters"]:
        if parameter["direction"] != "output":
            continue
        name = parameter["name"]
        length = parameter.get("len")
        count_name = str(length).split(",", 1)[0] if length else ""
        if count_name and count_name != "None":
            lines.extend(
                [
                    f"  if ({name} && response.{name} &&",
                    f"      response.{name} != {name} &&",
                    f"      response.{count_name}) {{",
                    f"    std::copy_n(response.{name},",
                    f"                *response.{count_name}, {name});",
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
    return "\n".join(fields)


def command_needs_response(command: dict[str, object]) -> bool:
    if str(command["return_type"]) != "void":
        return True
    return any(
        parameter["direction"] == "output" for parameter in command["parameters"]
    )


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

  static VkResult pack_response(Blob& blob,
                                const Response& response);
  static VkResult unpack_response(SafeArrayView<std::uint8_t>& view,
                                  const Response** response);
"""
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"
#include "generated/vulkan_manual_hooks.hpp"
#include "blob.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace vkfwd::generated::commands::{namespace} {{

struct Parameters {{
{fields}
}};
{response_struct}

class Command {{
public:
  using Parameters = vkfwd::generated::commands::{namespace}::Parameters;

  static VkResult pack_parameters(Blob& blob,
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
                        Blob& blob,
                        std::size_t pointer_slot_offset,
                        const VkAllocationCallbacks*& pointer_slot) {{
  (void)allocator;
  (void)blob;
  // Vulkan allocation callbacks are guest-process function pointers and user
  // data. They have no valid receiver-process address, so the wire contract is
  // to drop them and replay with the receiver's default allocator.
  return patch_command_pointer(pointer_slot, pointer_slot_offset, 0);
}}

template<class Pointer>
VkResult pack_output_pointer(Pointer value, Blob& blob, std::size_t pointer_slot_offset, Pointer& pointer_slot) {{
  using Pointee = std::remove_pointer_t<Pointer>;
  if (!value) [[unlikely]] {{ return patch_command_pointer(pointer_slot, pointer_slot_offset, 0); }}
  try {{
    std::size_t target = 0;
    auto destination = blob.grow<Pointee>(1, alignof(Pointee), &target);
    if (!destination.set(0, *value)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: could not copy output value into blob, size={{}}", sizeof(Pointee));
      return VK_ERROR_UNKNOWN;
    }}
    return patch_command_pointer(pointer_slot, pointer_slot_offset, target);
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command response pack failed: out of host memory while copying output value, size={{}}", sizeof(Pointee));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
}}

template<class T>
VkResult append_command_chunk(Blob& blob, CommandId command_id, std::uint32_t revision, const T& payload, CommandChunk& chunk, T*& packed_payload) {{
  constexpr std::size_t kPayloadOffset = command_payload_offset<T>();
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);
  constexpr std::size_t kPayloadAlignment = alignof(T);
  constexpr std::size_t kChunkAlignment =
      alignof(CommandChunkHeader) > kPayloadAlignment ? alignof(CommandChunkHeader) : kPayloadAlignment;

    // The chunk is one contiguous serialized range. Its fixed header is
    // command id, chunk size including the header, and command revision; payload
    // starts at an aligned offset after those fields.
  if constexpr (kCommandSize > std::numeric_limits<std::uint32_t>::max()) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: command chunk is too large, command_id={{}}, command_size={{}}",
                    static_cast<std::uint32_t>(command_id), kCommandSize);
    return VK_ERROR_UNKNOWN;
  }}

  chunk = CommandChunk{{.command_offset = 0, .command_size = 0}};
  packed_payload = nullptr;

  CommandChunkHeader header{{}};
  try {{
    std::size_t command_offset = 0;
    auto destination = blob.grow<std::uint8_t>(kCommandSize, kChunkAlignment, &command_offset);
    header.command_id = static_cast<std::uint32_t>(command_id);
    header.size = static_cast<std::uint32_t>(kCommandSize);
    header.command_revision = revision;

    if (destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t*>(&header)) != sizeof(header) ||
        destination.set(kPayloadOffset, sizeof(payload), reinterpret_cast<const std::uint8_t*>(&payload)) != sizeof(payload)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy command chunk, command_id={{}}, command_size={{}}",
                      static_cast<std::uint32_t>(command_id), kCommandSize);
      return VK_ERROR_UNKNOWN;
    }}
    chunk.command_offset = command_offset;
    chunk.command_size = header.size;
    packed_payload = reinterpret_cast<T*>(&destination.at(kPayloadOffset));
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={{}}, payload_size={{}}",
                    static_cast<std::uint32_t>(command_id), sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
  return VK_SUCCESS;
}}

template<class T>
VkResult append_command_chunk(Blob& blob, CommandId command_id, std::uint32_t revision, const T& payload, CommandChunk& chunk) {{
  T* packed_payload = nullptr;
  return append_command_chunk(blob, command_id, revision, payload, chunk, packed_payload);
}}

VkResult finalize_command_chunk(Blob& blob, CommandChunk& chunk) {{
  const std::size_t command_size = blob.size() - chunk.command_offset;
  if (command_size > std::numeric_limits<std::uint32_t>::max()) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: finalized command chunk is too large, command_offset={{}}, command_size={{}}",
                    chunk.command_offset, command_size);
    return VK_ERROR_UNKNOWN;
  }}

  auto header_view = blob.at<CommandChunkHeader>(chunk.command_offset);
  auto* header = header_view.address();
  if (!header) [[unlikely]] {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not rewrite command chunk size, command_offset={{}}", chunk.command_offset);
    return VK_ERROR_UNKNOWN;
  }}

  header->size = static_cast<std::uint32_t>(command_size);
  chunk.command_size = header->size;
  return VK_SUCCESS;
}}

template<class T>
VkResult unpack_command_chunk(SafeArrayView<std::uint8_t>& view, CommandId command_id, std::uint32_t revision, T** payload) {{
  constexpr std::size_t kPayloadOffset = command_payload_offset<T>();
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);
  auto* header = view.size() < sizeof(CommandChunkHeader) ? nullptr : reinterpret_cast<CommandChunkHeader*>(view.address(0));
  auto* packed_payload = view.size() < kCommandSize ? nullptr : reinterpret_cast<T*>(view.address(kPayloadOffset));
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
    return int(parameter["pointer_depth"]) == 1 and (
        str(parameter["type"])
        in {
            "VkInstanceCreateInfo",
            "VkDeviceCreateInfo",
            "VkAllocationCallbacks",
        }
        or parameter["direction"] == "output"
    )


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
                    f"  status = pack_allocator({source_name}.{name}, blob, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        elif parameter["direction"] == "output":
            lines.extend(
                [
                    f"  status = pack_output_pointer({source_name}.{name}, blob, {slot}, packed_parameters->{name});",
                    "  if (status != VK_SUCCESS) [[unlikely]] { return status; }",
                ]
            )
        else:
            lines.extend(
                [
                    f"  structure::PackedStruct packed_{name};",
                    f"  status = structure::pack_{ptype}({source_name}.{name}, blob, packed_{name});",
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
        if ptype in {"VkInstanceCreateInfo", "VkDeviceCreateInfo"}:
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
        lines.extend(
            [
                f"  status = pack_output_pointer(response.{name}, blob, {slot}, packed_response->{name});",
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
  CommandChunk chunk;
  VkResult status = append_command_chunk(blob, CommandId::{enum_name}, {COMMAND_REVISION}, {source_name}, chunk, packed_parameters);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  const std::size_t payload_offset = chunk.command_offset + command_payload_offset<Parameters>();
{pointer_lines}
  status = finalize_command_chunk(blob, chunk);
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
    structure_include = (
        '#include "generated/structure/core.hpp"' if needs_structure else ""
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
VkResult Command::pack_response(Blob& blob,
                                const Response& response) {{
  Response* packed_response = nullptr;
  CommandChunk chunk;
  VkResult status = append_command_chunk(blob, CommandId::{enum_name}, {COMMAND_REVISION}, response, chunk, packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  const std::size_t payload_offset = chunk.command_offset + command_payload_offset<Response>();
{response_pack_lines}
  status = finalize_command_chunk(blob, chunk);
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
#include <limits>
#include <new>
#include <type_traits>

namespace vkfwd::generated::commands::{namespace} {{
{helpers_block}

VkResult Command::pack_parameters(Blob& blob,
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

#include "protocol.hpp"

#include <cstdint>

namespace vkfwd::generated {{

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

}} // namespace vkfwd::generated
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
  static constexpr bool after_response_unpack_enabled = false;

  template <class... Args>
  static constexpr void before_pack(Args&...) noexcept {{}}

  template <class Parameters>
  static constexpr void after_response_unpack(Parameters&) noexcept {{}}
}};

}} // namespace vkfwd::forwarder::manual
"""


def dispatch_table_header_content(metadata: dict[str, object]) -> str:
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
  PFN_vkCreateInstance create_instance = nullptr;

  void init(PFN_vkGetInstanceProcAddr get_instance_proc_addr);
  PFN_vkVoidFunction getProcByName(const char* name) const;
}};

struct InstanceDispatchTable {{
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkDestroyInstance destroy_instance = nullptr;
  PFN_vkCreateDevice create_device = nullptr;

  void init(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr);
  PFN_vkVoidFunction getProcByName(const char* name) const;
}};

struct DeviceDispatchTable {{
  PFN_vkDestroyDevice destroy_device = nullptr;

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
    if name == "vkCreateInstance":
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


def dispatch_table_source_content(metadata: dict[str, object]) -> str:
    commands = list(metadata["commands"])
    pointer_map_entries = command_pointer_map_entries(commands)
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
  create_instance = get_instance_proc_addr
      ? typed_proc<PFN_vkCreateInstance>(get_instance_proc_addr(nullptr, "vkCreateInstance"))
      : nullptr;
}}

void InstanceDispatchTable::init(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr) {{
  // The instance table is populated only after vkCreateInstance succeeds. The
  // instance handle scopes extension/core lookup and must remain valid while
  // these dispatch slots are used.
  if (!get_instance_proc_addr) {{
    get_device_proc_addr = nullptr;
    destroy_instance = nullptr;
    create_device = nullptr;
    return;
  }}

  get_device_proc_addr = typed_proc<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr(instance, "vkGetDeviceProcAddr"));
  destroy_instance = typed_proc<PFN_vkDestroyInstance>(get_instance_proc_addr(instance, "vkDestroyInstance"));
  create_device = typed_proc<PFN_vkCreateDevice>(get_instance_proc_addr(instance, "vkCreateDevice"));
}}

void DeviceDispatchTable::init(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr) {{
  // Device commands are loaded after vkCreateDevice succeeds. The device
  // dispatch slots are scoped to that destination device and should be
  // refreshed for each replay/device mapping.
  if (!get_device_proc_addr) {{
    destroy_device = nullptr;
    return;
  }}

  destroy_device = typed_proc<PFN_vkDestroyDevice>(get_device_proc_addr(device, "vkDestroyDevice"));
}}

PFN_vkVoidFunction GlobalDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(get_instance_proc_addr);
  }}
  if (std::strcmp(name, "vkCreateInstance") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
  }}
  return nullptr;
}}

PFN_vkVoidFunction InstanceDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(get_device_proc_addr);
  }}
  if (std::strcmp(name, "vkDestroyInstance") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
  }}
  if (std::strcmp(name, "vkCreateDevice") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(create_device);
  }}
  return nullptr;
}}

PFN_vkVoidFunction DeviceDispatchTable::getProcByName(const char* name) const {{
  if (!name) {{ return nullptr; }}
  if (std::strcmp(name, "vkDestroyDevice") == 0) {{
    return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
  }}
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
    return f"""#include "generated/forwarder_entrypoints.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);

namespace vkfwd::forwarder::generated {{
namespace {{

const ::vkfwd::generated::GlobalDispatchTable kGlobalDispatchTable {{
  .get_instance_proc_addr = vkGetInstanceProcAddr,
  .create_instance = {forwarder_entrypoint_name("vkCreateInstance")},
}};

const ::vkfwd::generated::InstanceDispatchTable kInstanceDispatchTable {{
  .get_device_proc_addr = vkGetDeviceProcAddr,
  .destroy_instance = {forwarder_entrypoint_name("vkDestroyInstance")},
  .create_device = {forwarder_entrypoint_name("vkCreateDevice")},
}};

const ::vkfwd::generated::DeviceDispatchTable kDeviceDispatchTable {{
  .destroy_device = {forwarder_entrypoint_name("vkDestroyDevice")},
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


def forwarder_command_source_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    return_statement = response_return_statement(command)
    failure_return = status_failure_return_statement(command, "status")
    output_assignments = output_parameter_assignments(command)
    if output_assignments:
        output_assignments = "\n" + output_assignments + "\n"
    if command_needs_response(command):
        response_flow = f"""
  Blob response_blob = forwarder.flush();
  auto response_view = response_blob.at(0, response_blob.size());
  const Command::Response* packed_response = nullptr;
  status = Command::unpack_response(response_view, &packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{
{failure_return}
  }}
  Command::Response response = *packed_response;

  if constexpr (Hooks::after_response_unpack_enabled) {{
    Hooks::after_response_unpack(response);
  }}
{output_assignments}
  // Synchronous forwarding flushes this thread's pending request blob and
  // returns a fresh response blob. Generated code only decodes that blob here;
  // transport implementations own delivery, replay, and handle mapping policy.
"""
    else:
        response_flow = f"""
  // Deferrable commands have no return value or output parameters, so the
  // entry point only appends to the thread-local request blob. The next
  // non-deferrable command is responsible for flushing this thread's pending
  // command sequence through the transport session.
"""
    return f"""#include "generated/forwarder_entrypoints.hpp"

#include "forwarder.hpp"
#include "generated/command/{command['name']}.hpp"
#include "generated/forwarder_hooks.hpp"

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
  VkResult status = Command::pack_parameters(forwarder.request_blob(), parameters);
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


def receiver_endpoint_declarations(metadata: dict[str, object]) -> str:
    return "\n".join(
        "bool "
        f"{receiver_endpoint_function_name(command)}(const Blob& request_blob, const CommandChunk& request_packet, Blob& response_blob, "
        "::vkfwd::receiver::ReplayContext& replay_context);"
        for command in metadata["commands"]
    )


def receiver_endpoint_source(command: dict[str, object]) -> str:
    namespace = command_namespace(str(command["name"]))
    enum_name = command_enum_name(str(command["name"]))
    parameter_names = ", ".join(
        f"parameters->{parameter['name']}" for parameter in command["parameters"]
    )
    pfn_type = command_pfn_type(str(command["name"]))

    call_lines = []
    if str(command["return_type"]) == "void":
        call_lines.append(f"  api_function({parameter_names});")
    else:
        call_lines.append(
            f"  const {command['return_type']} return_value = api_function({parameter_names});"
        )

    if command_needs_response(command):
        call_lines.extend(
            [
                f"  Command::Response response {receiver_response_initializer(command)};",
                "  return Command::pack_response(response_blob, response) == VK_SUCCESS;",
            ]
        )
    else:
        call_lines.append("  return true;")

    return f"""bool {receiver_endpoint_function_name(command)}(const Blob& request_blob, const CommandChunk& request_packet, Blob& response_blob,
                               ::vkfwd::receiver::ReplayContext& replay_context) {{
  using Command = ::vkfwd::generated::commands::{namespace}::Command;

  const auto raw_function = replay_context.dispatch.getProcByCommandId(::vkfwd::generated::CommandId::{enum_name});
  if (!raw_function) {{ return false; }}
  const auto api_function = reinterpret_cast<{pfn_type}>(raw_function);

  auto& mutable_request_blob = const_cast<Blob&>(request_blob);
  auto request_view = mutable_request_blob.at(request_packet.command_offset, request_packet.command_size);
  const Command::Parameters* parameters = nullptr;
  if (Command::unpack_parameters(request_view, &parameters) != VK_SUCCESS) {{ return false; }}
{chr(10).join(call_lines)}
}}
"""


def receiver_endpoint_dispatch_cases(metadata: dict[str, object]) -> str:
    return "\n".join(
        f"  case ::vkfwd::generated::CommandId::{command_enum_name(str(command['name']))}:\n"
        f"    return {receiver_endpoint_function_name(command)}(request_blob, request_packet, response_blob, replay_context);"
        for command in metadata["commands"]
    )


def receiver_endpoints_header_content(metadata: dict[str, object]) -> str:
    declarations = receiver_endpoint_declarations(metadata)
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "blob.hpp"
#include "generated/dispatch_table.hpp"
#include "protocol.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::generated {{

{declarations}

bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const Blob& request_blob, const CommandChunk& request_packet, Blob& response_blob,
                       ::vkfwd::receiver::ReplayContext& replay_context);

}} // namespace vkfwd::receiver::generated
"""


def receiver_endpoints_source_content(metadata: dict[str, object]) -> str:
    includes = "\n".join(
        f'#include "generated/command/{command["name"]}.hpp"'
        for command in metadata["commands"]
    )
    endpoints = "\n".join(
        receiver_endpoint_source(command) for command in metadata["commands"]
    )
    dispatch_cases = receiver_endpoint_dispatch_cases(metadata)
    return f"""#include "generated/endpoints.hpp"

{includes}

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::receiver::generated {{

{endpoints}
bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const Blob& request_blob, const CommandChunk& request_packet, Blob& response_blob,
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
    (receiver_dir / "endpoints.hpp").write_text(
        receiver_endpoints_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.cpp").write_text(
        receiver_endpoints_source_content(metadata), encoding="utf-8"
    )


def structure_test_support_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "blob.hpp"

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

inline SafeArrayView<std::uint8_t> view_from(Blob & blob, std::size_t offset) {{
    return blob.at(offset, blob.size() - offset);
}}

template<class Pointer>
bool points_into(SafeArrayView<std::uint8_t> & view, Pointer pointer) {{
    auto * begin = view.address(0);
    if (!begin || !pointer) {{ return false; }}
    const auto * target = reinterpret_cast<const std::uint8_t *>(pointer);
    return target >= begin && target < begin + view.size();
}}

template<class Pointer>
bool points_into_blob(Blob & blob, Pointer pointer) {{
    auto view = blob.at(0, blob.size());
    return points_into(view, pointer);
}}

template<class T>
const T & object_at(const Blob & blob, std::size_t offset) {{
    const auto view = blob.at(offset, sizeof(T));
    REQUIRE(!view.empty());
    return *reinterpret_cast<const T *>(&view.at(0));
}}

inline void check_relative_string(Blob & blob, std::size_t, const char * value, std::string_view expected) {{
    REQUIRE(value != nullptr);
    CHECK(points_into_blob(blob, value));
    CHECK(std::string_view(value, expected.size()) == expected);
    CHECK(value[expected.size()] == '\\0');
}}

inline void check_relative_string_array(Blob & blob, std::size_t base_offset, const char * const * encoded_values,
                                        std::initializer_list<std::string_view> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    CHECK(points_into_blob(blob, encoded_values));
    std::size_t index = 0;
    for (std::string_view expected_value : expected) {{
        const auto * actual_value = encoded_values[index];
        REQUIRE(actual_value != nullptr);
        CHECK(points_into_blob(blob, actual_value));
        CHECK(std::string_view(actual_value, expected_value.size()) == expected_value);
        CHECK(actual_value[expected_value.size()] == '\\0');
        ++index;
    }}
}}

template<class T>
void check_relative_plain_array(Blob & blob, std::size_t base_offset, const T * encoded_values, std::initializer_list<T> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    CHECK(points_into_blob(blob, encoded_values));
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
    Blob blob;
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

    REQUIRE(pack_VkApplicationInfo(&value, blob, packed) == VK_SUCCESS);
    const VkApplicationInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
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

    REQUIRE(pack_VkInstanceCreateInfo(&value, blob, packed) == VK_SUCCESS);
    const VkInstanceCreateInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
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

    REQUIRE(pack_VkDeviceQueueCreateInfo(&value, blob, packed) == VK_SUCCESS);
    const VkDeviceQueueCreateInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
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

    REQUIRE(pack_VkDeviceCreateInfo(&value, blob, packed) == VK_SUCCESS);
    const VkDeviceCreateInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
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

    REQUIRE(pack_VkDeviceGroupDeviceCreateInfo(&value, blob, packed) == VK_SUCCESS);
    const VkDeviceGroupDeviceCreateInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkDeviceQueueGlobalPriorityCreateInfo value {{
        .sType          = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,
        .pNext          = nullptr,
        .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH,
    }};

    REQUIRE(pack_VkDeviceQueueGlobalPriorityCreateInfo(&value, blob, packed) == VK_SUCCESS);
    const VkDeviceQueueGlobalPriorityCreateInfo * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceFeatures2 value {{
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = nullptr,
        .features = {{}},
    }};
    value.features.robustBufferAccess = VK_TRUE;
    value.features.geometryShader     = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceFeatures2(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceFeatures2 * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceVulkan11Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = nullptr,
    }};
    value.storageBuffer16BitAccess = VK_TRUE;
    value.shaderDrawParameters     = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan11Features(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan11Features * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceVulkan12Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = nullptr,
    }};
    value.descriptorIndexing = VK_TRUE;
    value.timelineSemaphore  = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan12Features(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan12Features * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceVulkan13Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = nullptr,
    }};
    value.synchronization2  = VK_TRUE;
    value.dynamicRendering = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan13Features(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan13Features * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceVulkan14Features value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = nullptr,
    }};
    value.globalPriorityQuery = VK_TRUE;
    value.maintenance6        = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceVulkan14Features(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceVulkan14Features * actual = nullptr;
    Blob flattened = blob.flatten();
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
    Blob blob;
    PackedStruct packed;
    VkPhysicalDeviceDescriptorIndexingFeatures value {{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .pNext = nullptr,
    }};
    value.descriptorBindingPartiallyBound        = VK_TRUE;
    value.descriptorBindingVariableDescriptorCount = VK_TRUE;

    REQUIRE(pack_VkPhysicalDeviceDescriptorIndexingFeatures(&value, blob, packed) == VK_SUCCESS);
    const VkPhysicalDeviceDescriptorIndexingFeatures * actual = nullptr;
    Blob flattened = blob.flatten();
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


def command_roundtrip_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "blob.hpp"
#include "generated/command/vkCreateDevice.hpp"
#include "generated/command/vkCreateInstance.hpp"
#include "generated/command/vkDestroyDevice.hpp"
#include "generated/command/vkDestroyInstance.hpp"
#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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

SafeArrayView<std::uint8_t> full_view(Blob & blob) {{
    return blob.at(0, blob.size());
}}

SafeArrayView<std::uint8_t> tail_view(Blob & blob, std::size_t offset) {{
    return blob.at(offset, blob.size() - offset);
}}

template<class T>
constexpr std::size_t command_payload_offset() {{
    constexpr std::size_t alignment = alignof(T);
    return (sizeof(CommandChunkHeader) + alignment - 1) & ~(alignment - 1);
}}

template<class T>
const T * packed_command_payload(Blob & blob) {{
    const auto view = blob.at(command_payload_offset<T>(), sizeof(T));
    REQUIRE(!view.empty());
    return reinterpret_cast<const T *>(view.address(0));
}}

void check_string(const char * actual, std::string_view expected) {{
    REQUIRE(actual != nullptr);
    CHECK(std::string_view(actual, expected.size()) == expected);
    CHECK(actual[expected.size()] == '\\0');
}}

template<class T>
void check_array(const T * actual, std::initializer_list<T> expected) {{
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

}} // namespace

TEST_CASE("generated vkCreateInstance parameter pack flatten unpack reconstructs every pointer into flattened blob") {{
    using Command = commands::vkCreateInstance::Command;
    Blob blob(64);
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

    REQUIRE(Command::pack_parameters(blob, parameters) == VK_SUCCESS);
    Blob flattened = blob.flatten();
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

TEST_CASE("generated vkCreateDevice parameter pack flatten unpack reconstructs every pointer into flattened blob") {{
    using Command = commands::vkCreateDevice::Command;
    Blob blob(64);
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

    REQUIRE(Command::pack_parameters(blob, parameters) == VK_SUCCESS);
    Blob flattened = blob.flatten();
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
    check_array(actual->pCreateInfo->pQueueCreateInfos->pQueuePriorities, {{0.25f, 0.75f}});
    CHECK(actual->pCreateInfo->pEnabledFeatures->robustBufferAccess == VK_TRUE);
}}

TEST_CASE("generated destroy command parameter pack flatten unpack drops allocator callbacks") {{
    int allocator_user_data = 0x51;
    auto allocator = test_allocator(&allocator_user_data);

    {{
        using Command = commands::vkDestroyInstance::Command;
        Blob blob(64);
        Command::Parameters parameters {{
            .instance   = test_handle<VkInstance>(0x601),
            .pAllocator = &allocator,
        }};
        REQUIRE(Command::pack_parameters(blob, parameters) == VK_SUCCESS);
        Blob flattened = blob.flatten();
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
        Blob blob(64);
        Command::Parameters parameters {{
            .device     = test_handle<VkDevice>(0x602),
            .pAllocator = &allocator,
        }};
        REQUIRE(Command::pack_parameters(blob, parameters) == VK_SUCCESS);
        Blob flattened = blob.flatten();
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

TEST_CASE("generated create command responses pack flatten unpack reconstruct output pointers into flattened blob") {{
    {{
        using Command = commands::vkCreateInstance::Command;
        Blob blob(64);
        VkInstance instance = test_handle<VkInstance>(0x701);
        Command::Response response {{
            .return_value = VK_SUCCESS,
            .pInstance    = &instance,
        }};
        REQUIRE(Command::pack_response(blob, response) == VK_SUCCESS);
        Blob flattened = blob.flatten();
        auto view = full_view(flattened);
        const Command::Response * actual = nullptr;
        REQUIRE(Command::unpack_response(view, &actual) == VK_SUCCESS);
        REQUIRE(points_into(view, actual));
        REQUIRE(points_into(view, actual->pInstance));
        CHECK(*actual->pInstance == instance);
    }}

    {{
        using Command = commands::vkCreateDevice::Command;
        Blob blob(64);
        VkDevice device = test_handle<VkDevice>(0x702);
        Command::Response response {{
            .return_value = VK_SUCCESS,
            .pDevice      = &device,
        }};
        REQUIRE(Command::pack_response(blob, response) == VK_SUCCESS);
        Blob flattened = blob.flatten();
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
        "handles": {
            name: handles[name]
            for name in ("VkInstance", "VkPhysicalDevice", "VkDevice")
        },
        "structs": collect_structs(root, command_structs),
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    write_manifest(metadata, output_dir / "vulkan_manifest.json")
    write_coverage(metadata, output_dir / "vulkan_coverage.md")
    write_manual_hooks_header(metadata, output_dir / "vulkan_manual_hooks.hpp")
    write_command_files(metadata, output_dir)
    write_vulkan_api_header(metadata, output_dir / "vulkan_api.hpp")
    write_dispatch_table_files(metadata, output_dir)
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
