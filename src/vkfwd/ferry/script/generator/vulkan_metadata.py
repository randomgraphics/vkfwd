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

  template <class ParameterPacket>
  static constexpr void after_pack(ParameterPacket&) noexcept {{}}

  template <class ParameterPacket>
  static constexpr void before_unpack(ParameterPacket&) noexcept {{}}

  template <class Parameters>
  static constexpr void after_unpack(Parameters&) noexcept {{}}
}};

}} // namespace vkfwd::manual
"""
    path.write_text(content, encoding="utf-8")


def command_header_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    if command["name"] in {"vkCreateInstance", "vkCreateDevice"}:
        # The create-command stream layout is being reorganized around Blob and
        # generated structure helpers. Until the general emitter learns that
        # policy for every command, keep this initial generated slice
        # deterministic by using the checked-in generated template verbatim.
        return (
            repo_root()
            / "src"
            / "vkfwd"
            / "ferry"
            / "core"
            / "generated"
            / "command"
            / f"{command['name']}.hpp"
        ).read_text(encoding="utf-8")
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    fields = "\n".join(
        f"  {parameter_cxx_type(parameter)} {parameter['name']} = {{}};"
        for parameter in command["parameters"]
    )
    response_aliases = ""
    response_methods = ""
    response_struct = ""
    if command_needs_response(command):
        response_struct = f"""
struct Response {{
{response_return_member(command)}{response_output_fields(command)}
}};
"""
        response_aliases = f"""using ResponsePacket = vkfwd::CommandChunk;
"""
        response_methods = f"""
  using Response = vkfwd::generated::commands::{namespace}::Response;
  using ResponsePacket = vkfwd::generated::commands::{namespace}::ResponsePacket;

  static VkResult pack_response(Blob& blob,
                                const Response& response,
                                ResponsePacket& packet);
  static VkResult unpack_response(Blob& blob,
                                  const ResponsePacket& packet,
                                  Response& response);
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

namespace vkfwd::generated::commands::{namespace} {{

struct Parameters {{
{fields}
}};
{response_struct}
using ParameterPacket = vkfwd::CommandChunk;
{response_aliases}

class Command {{
public:
  using Parameters = vkfwd::generated::commands::{namespace}::Parameters;
  using ParameterPacket = vkfwd::generated::commands::{namespace}::ParameterPacket;

  static VkResult pack_parameters(Blob& blob,
                                  const Parameters& parameters,
                                  ParameterPacket& packet);
  static VkResult unpack_parameters(Blob& blob,
                                    const ParameterPacket& packet,
                                    Parameters& parameters);
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
VkResult append_command_chunk(Blob& blob, CommandId command_id, std::uint32_t revision, const T& payload, CommandChunk& chunk) {{
  constexpr std::size_t kPayloadAlignment = alignof(T);
  constexpr std::size_t kPayloadOffset =
      (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);
  constexpr std::size_t kChunkAlignment =
      alignof(CommandChunkHeader) > kPayloadAlignment ? alignof(CommandChunkHeader) : kPayloadAlignment;

  // The chunk is one contiguous serialized range. Payload starts at an aligned
  // offset inside that range so receivers can safely reinterpret the packed
  // command bytes without depending on host-side append history.
  if constexpr (kCommandSize > std::numeric_limits<std::uint32_t>::max()) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: command chunk is too large, command_id={{}}, command_size={{}}",
                    static_cast<std::uint32_t>(command_id), kCommandSize);
    return VK_ERROR_UNKNOWN;
  }}

  chunk = CommandChunk{{.command_offset = 0, .command_size = 0}};

  CommandChunkHeader header{{}};
  try {{
    auto destination = blob.grow<std::uint8_t>(kCommandSize, kChunkAlignment);
    header.command_id = static_cast<std::uint32_t>(command_id);
    header.size = static_cast<std::uint32_t>(kCommandSize);
    header.command_revision = revision;

    if (destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t*>(&header)) != sizeof(header) ||
        destination.set(kPayloadOffset, sizeof(payload), reinterpret_cast<const std::uint8_t*>(&payload)) != sizeof(payload)) [[unlikely]] {{
      VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy command chunk, command_id={{}}, command_size={{}}",
                      static_cast<std::uint32_t>(command_id), kCommandSize);
      return VK_ERROR_UNKNOWN;
    }}
    chunk.command_offset = destination.offset();
    chunk.command_size = header.size;
  }} catch (const std::bad_alloc&) {{
    VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={{}}, payload_size={{}}",
                    static_cast<std::uint32_t>(command_id), sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}
  return VK_SUCCESS;
}}

template<class T>
VkResult unpack_command_chunk(const Blob& blob, const CommandChunk& chunk, CommandId command_id, std::uint32_t revision, const T** payload) {{
  constexpr std::size_t kPayloadAlignment = alignof(T);
  constexpr std::size_t kPayloadOffset =
      (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
  constexpr std::size_t kCommandSize = kPayloadOffset + sizeof(T);
  const auto header_view = blob.data_at(chunk.command_offset, sizeof(CommandChunkHeader));
  const auto payload_view = blob.data_at(chunk.command_offset + kPayloadOffset, sizeof(T));
  const auto* header = reinterpret_cast<const CommandChunkHeader*>(header_view.data());
  const auto* packed_payload = reinterpret_cast<const T*>(payload_view.data());
  if (!header || !packed_payload || header->command_id != static_cast<std::uint32_t>(command_id) || header->command_revision != revision ||
      header->size != chunk.command_size || chunk.command_size != kCommandSize) [[unlikely]] {{
    VKFWD_LOG_ERROR(
        "vkfwd ferry command unpack failed: invalid command chunk, offset={{}}, size={{}}, has_header={{}}, has_payload={{}}, command_id={{}}, "
        "expected_command_id={{}}, revision={{}}, expected_revision={{}}, header_size={{}}, expected_size={{}}",
        chunk.command_offset, chunk.command_size, header != nullptr, packed_payload != nullptr, header ? header->command_id : 0,
        static_cast<std::uint32_t>(command_id), header ? header->command_revision : 0, revision, header ? header->size : 0, kCommandSize);
    return VK_ERROR_UNKNOWN;
  }}

  *payload = packed_payload;
  return VK_SUCCESS;
}}
"""


def command_pack_body(command: dict[str, object], enum_name: str) -> str:
    return f"""
    VkResult status = append_command_chunk(blob, CommandId::{enum_name}, {COMMAND_REVISION}, {{PARAM}}, packet);
    if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
"""


def command_source_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    if command["name"] in {"vkCreateInstance", "vkCreateDevice"}:
        # See command_header_content(): this preserves deterministic generation
        # for the first Blob-backed command slice while the all-command emitter
        # is still catching up to the new stream contract.
        return (
            repo_root()
            / "src"
            / "vkfwd"
            / "ferry"
            / "core"
            / "generated"
            / "command"
            / f"{command['name']}.cpp"
        ).read_text(encoding="utf-8")
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    helpers = command_source_helpers(command)
    helpers_block = (
        f"""namespace {{

{helpers}

}} // namespace
"""
        if helpers.strip()
        else ""
    )
    pack_body = command_pack_body(command, enum_name)
    response_methods = ""
    if command_needs_response(command):
        response_methods = f"""
VkResult Command::pack_response(Blob& blob,
                                const Response& response,
                                ResponsePacket& packet) {{
  return append_command_chunk(blob, CommandId::{enum_name}, {COMMAND_REVISION}, response, packet);
}}

VkResult Command::unpack_response(Blob& blob,
                                  const ResponsePacket& packet,
                                  Response& response) {{
  const Response* packed_response = nullptr;
  VkResult status = unpack_command_chunk(blob, packet, CommandId::{enum_name}, {COMMAND_REVISION}, &packed_response);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  response = *packed_response;
  return VK_SUCCESS;
}}
"""
    return f"""#include "generated/command/{command['name']}.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "logging.hpp"

#include <cstdint>
#include <limits>
#include <new>

namespace vkfwd::generated::commands::{namespace} {{
{helpers_block}

VkResult Command::pack_parameters(Blob& blob,
                                  const Parameters& parameters,
                                  ParameterPacket& packet) {{
  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_pack_enabled) {{
    Parameters hook_parameters = parameters;
    Hooks::before_pack(hook_parameters);

{pack_body.replace("{PARAM}", "hook_parameters")}

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack(packet);
    }}
    return VK_SUCCESS;
  }} else {{
{pack_body.replace("{PARAM}", "parameters")}

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack(packet);
    }}
    return VK_SUCCESS;
  }}
}}

VkResult Command::unpack_parameters(Blob& blob,
                                    const ParameterPacket& packet,
                                    Parameters& parameters) {{
  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_unpack_enabled) {{
    Hooks::before_unpack(packet);
  }}

  const Parameters* packed_parameters = nullptr;
  VkResult status = unpack_command_chunk(blob, packet, CommandId::{enum_name}, {COMMAND_REVISION}, &packed_parameters);
  if (status != VK_SUCCESS) [[unlikely]] {{ return status; }}
  parameters = *packed_parameters;

  if constexpr (Hooks::after_unpack_enabled) {{
    Hooks::after_unpack(parameters);
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
and the pinned Vulkan API version. There is intentionally no generated
`vulkan_api.cpp`; command metadata and behavior stay per-command.
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


def forwarder_header_content(metadata: dict[str, object]) -> str:
    declarations = "\n".join(
        f"VKAPI_ATTR {command['return_type']} VKAPI_CALL {command['name']}(\n"
        f"    {command_parameter_declarations(command)});"
        for command in metadata["commands"]
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

#include <vulkan/vulkan.h>

namespace vkfwd::forwarder::generated {{

{declarations}

struct GlobalDispatchTable {{
  PFN_vkCreateInstance create_instance = vkCreateInstance;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
}};

struct InstanceDispatchTable {{
  PFN_vkDestroyInstance destroy_instance = vkDestroyInstance;
  PFN_vkCreateDevice create_device = vkCreateDevice;
}};

struct DeviceDispatchTable {{
  PFN_vkDestroyDevice destroy_device = vkDestroyDevice;
}};

const GlobalDispatchTable& global_dispatch_table();
const InstanceDispatchTable& instance_dispatch_table();
const DeviceDispatchTable& device_dispatch_table();

}} // namespace vkfwd::forwarder::generated
"""


def forwarder_tables_source_content(metadata: dict[str, object]) -> str:
    return f"""#include "generated/dispatch_table.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::forwarder::generated {{
namespace {{

const GlobalDispatchTable kGlobalDispatchTable;
const InstanceDispatchTable kInstanceDispatchTable;
const DeviceDispatchTable kDeviceDispatchTable;

}} // namespace

const GlobalDispatchTable& global_dispatch_table() {{
  return kGlobalDispatchTable;
}}

const InstanceDispatchTable& instance_dispatch_table() {{
  return kInstanceDispatchTable;
}}

const DeviceDispatchTable& device_dispatch_table() {{
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
  Command::ResponsePacket response_packet;
  response_packet.command_offset = 0;
  response_packet.command_size = static_cast<std::uint32_t>(response_blob.size());

  Command::Response response;
  status = Command::unpack_response(response_blob, response_packet, response);
  if (status != VK_SUCCESS) [[unlikely]] {{
{failure_return}
  }}

  if constexpr (Hooks::after_response_unpack_enabled) {{
    Hooks::after_response_unpack(response);
  }}
{output_assignments}
  // Synchronous forwarding flushes this thread's pending request blob and
  // returns a fresh response blob. Generated code only decodes that blob here;
  // channel implementations own transport, replay, and handle mapping policy.
"""
    else:
        response_flow = f"""
  // Deferrable commands have no return value or output parameters, so the
  // entry point only appends to the thread-local request blob. The next
  // non-deferrable command is responsible for flushing this thread's pending
  // command sequence through the channel.
"""
    return f"""#include "generated/dispatch_table.hpp"

#include "forwarder.hpp"
#include "generated/command/{command['name']}.hpp"
#include "generated/vulkan_forwarder_hooks.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <algorithm>
#include <cstdint>

#if __has_include("hook/{command['name']}ForwarderHook.hpp")
#include "hook/{command['name']}ForwarderHook.hpp"
#endif

namespace vkfwd::forwarder::generated {{

VKAPI_ATTR {command['return_type']} VKAPI_CALL {command['name']}(
    {command_parameter_declarations(command)}) {{
  using Command = ::vkfwd::generated::commands::{namespace}::Command;
  using Hooks = ::vkfwd::forwarder::manual::CommandHooks<
      ::vkfwd::generated::CommandId::{enum_name}>;

  if constexpr (Hooks::before_pack_enabled) {{
    Hooks::before_pack({command_parameter_names(command)});
  }}

  auto& forwarder = ::vkfwd::Forwarder::instance();
  Command::Parameters parameters{parameter_initializer_list(command)};
  Command::ParameterPacket request;
  VkResult status = Command::pack_parameters(forwarder.request_blob(), parameters, request);
  if (status != VK_SUCCESS) [[unlikely]] {{
{failure_return}
  }}
{response_flow}
{return_statement}
}}

}} // namespace vkfwd::forwarder::generated
"""


def write_forwarder_files(metadata: dict[str, object], forwarder_dir: Path) -> None:
    commands_dir = forwarder_dir / "command"
    commands_dir.mkdir(parents=True, exist_ok=True)
    (forwarder_dir / "dispatch_table.hpp").write_text(
        forwarder_header_content(metadata), encoding="utf-8"
    )
    (forwarder_dir / "dispatch_table.cpp").write_text(
        forwarder_tables_source_content(metadata), encoding="utf-8"
    )
    (forwarder_dir / "vulkan_forwarder_hooks.hpp").write_text(
        forwarder_hooks_header_content(metadata), encoding="utf-8"
    )
    for command in metadata["commands"]:
        (commands_dir / f"{command['name']}.cpp").write_text(
            forwarder_command_source_content(metadata, command), encoding="utf-8"
        )


def forwarder_test_support_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "blob.hpp"
#include "forwarder.hpp"
#include "protocol.hpp"
#include "transport_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace vkfwd::forwarder::generated::test {{

using FlushHandler = Blob (*)(Blob & request_blob);

struct ChannelState {{
    FlushHandler handler   = nullptr;
    bool         processed = false;
}};

inline ChannelState & channel_state() {{
    static ChannelState state;
    return state;
}}

class PackUnpackChannel final: public TransportChannel {{
public:
    Blob send(Blob & request_blob) override {{
        auto & state   = channel_state();
        state.processed = true;
        REQUIRE(state.handler != nullptr);
        return state.handler(request_blob);
    }}
}};

inline std::unique_ptr<TransportChannel> make_pack_unpack_channel() {{ return std::make_unique<PackUnpackChannel>(); }}

inline void install_pack_unpack_channel(FlushHandler handler) {{
    auto & state    = channel_state();
    state.handler   = handler;
    state.processed = false;
    Forwarder::set_channel_creator(make_pack_unpack_channel);
    Forwarder::instance().request_blob().reset();
}}

inline CommandChunk first_command_chunk(const Blob & request_blob) {{
    // Channel tests reconstruct the packet metadata from the stream header
    // because the forwarding boundary only transports blob bytes, not the
    // caller-side CommandChunk wrapper returned by pack_parameters().
    const auto header_view = request_blob.data_at(0, sizeof(CommandChunkHeader));
    REQUIRE(header_view.data() != nullptr);
    const auto * header = reinterpret_cast<const CommandChunkHeader *>(header_view.data());
    return CommandChunk {{.command_offset = 0, .command_size = header->size}};
}}

template<class Pointer>
std::size_t encoded_offset(Pointer pointer) {{
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(pointer));
}}

template<class T>
const T & object_at(const Blob & blob, std::size_t offset) {{
    const auto view = blob.data_at(offset, sizeof(T));
    REQUIRE(view.data() != nullptr);
    return *reinterpret_cast<const T *>(view.data());
}}

inline void check_relative_string(const Blob & blob, std::size_t base_offset, const char * encoded_value, std::string_view expected) {{
    REQUIRE(encoded_value != nullptr);
    const std::size_t string_offset = base_offset + encoded_offset(encoded_value);
    const auto        view          = blob.data_at(string_offset, expected.size() + 1);
    REQUIRE(view.data() != nullptr);
    const auto * value = reinterpret_cast<const char *>(view.data());
    CHECK(std::string_view(value, expected.size()) == expected);
    CHECK(value[expected.size()] == '\\0');
}}

inline void check_relative_string_array(const Blob & blob,
                                        std::size_t base_offset,
                                        const char * const * encoded_values,
                                        std::initializer_list<std::string_view> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    const std::size_t array_offset = base_offset + encoded_offset(encoded_values);
    const auto        slots_view   = blob.data_at(array_offset, expected.size() * sizeof(std::uintptr_t));
    REQUIRE(slots_view.data() != nullptr);
    const auto * slots = reinterpret_cast<const std::uintptr_t *>(slots_view.data());

    std::size_t index = 0;
    for (std::string_view value : expected) {{
        REQUIRE(slots[index] != 0);
        const auto string_view = blob.data_at(base_offset + static_cast<std::size_t>(slots[index]), value.size() + 1);
        REQUIRE(string_view.data() != nullptr);
        const auto * string = reinterpret_cast<const char *>(string_view.data());
        CHECK(std::string_view(string, value.size()) == value);
        CHECK(string[value.size()] == '\\0');
        ++index;
    }}
}}

inline void check_allocator_callbacks(const VkAllocationCallbacks & actual, const VkAllocationCallbacks & expected) {{
    CHECK(actual.pUserData == expected.pUserData);
    CHECK(actual.pfnAllocation == expected.pfnAllocation);
    CHECK(actual.pfnReallocation == expected.pfnReallocation);
    CHECK(actual.pfnFree == expected.pfnFree);
    CHECK(actual.pfnInternalAllocation == expected.pfnInternalAllocation);
    CHECK(actual.pfnInternalFree == expected.pfnInternalFree);
}}

inline void * VKAPI_PTR test_allocation(void *, std::size_t, std::size_t, VkSystemAllocationScope) {{ return nullptr; }}

inline void * VKAPI_PTR test_reallocation(void *, void *, std::size_t, std::size_t, VkSystemAllocationScope) {{ return nullptr; }}

inline void VKAPI_PTR test_free(void *, void *) {{}}

inline void VKAPI_PTR test_internal_allocation(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {{}}

inline void VKAPI_PTR test_internal_free(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {{}}

inline VkAllocationCallbacks test_allocator(void * user_data) {{
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

}} // namespace vkfwd::forwarder::generated::test
"""


def vkcreateinstance_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/command/vkCreateInstance.hpp"
#include "generated/dispatch_table.hpp"
#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::forwarder::generated::test {{
namespace {{

using Command = ::vkfwd::generated::commands::vkCreateInstance::Command;

struct Scenario {{
    int                              allocator_user_data = 0x31;
    VkAllocationCallbacks            allocator;
    VkApplicationInfo                application_info;
    std::array<const char *, 2>       layers;
    std::array<const char *, 2>       extensions;
    VkInstanceCreateInfo             create_info;
    VkInstance *                     output_instance = nullptr;
    VkInstance                       response_instance;
    VkResult                         response_result = VK_INCOMPLETE;
}};

Scenario make_scenario() {{
    Scenario scenario;
    scenario.allocator = test_allocator(&scenario.allocator_user_data);
    scenario.application_info = VkApplicationInfo {{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "vkfwd-create-instance-app",
        .applicationVersion = 7,
        .pEngineName        = "vkfwd-create-instance-engine",
        .engineVersion      = 11,
        .apiVersion         = VK_MAKE_API_VERSION(0, 1, 2, 3),
    }};
    scenario.layers     = {{"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"}};
    scenario.extensions = {{"VK_EXT_debug_utils", "VK_KHR_surface"}};
    scenario.create_info = VkInstanceCreateInfo {{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkInstanceCreateFlags {{0x4}},
        .pApplicationInfo        = &scenario.application_info,
        .enabledLayerCount       = static_cast<std::uint32_t>(scenario.layers.size()),
        .ppEnabledLayerNames     = scenario.layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(scenario.extensions.size()),
        .ppEnabledExtensionNames = scenario.extensions.data(),
    }};
    scenario.response_instance = test_handle<VkInstance>(0x101);
    return scenario;
}}

Scenario & scenario() {{
    static Scenario value = make_scenario();
    return value;
}}

Blob handle_flush(Blob & request_blob) {{
    auto & expected = scenario();
    const auto packet = first_command_chunk(request_blob);

    Command::Parameters actual;
    REQUIRE(Command::unpack_parameters(request_blob, packet, actual) == VK_SUCCESS);
    CHECK(actual.pInstance == expected.output_instance);

    REQUIRE(actual.pCreateInfo != nullptr);
    const auto create_info_offset = encoded_offset(actual.pCreateInfo);
    const VkInstanceCreateInfo * packed_create_info = nullptr;
    REQUIRE(::vkfwd::generated::structure::unpack_VkInstanceCreateInfo(request_blob, create_info_offset, &packed_create_info) == VK_SUCCESS);
    REQUIRE(packed_create_info != nullptr);
    CHECK(packed_create_info->pNext == nullptr);
    CHECK(packed_create_info->flags == expected.create_info.flags);
    CHECK(packed_create_info->enabledLayerCount == expected.create_info.enabledLayerCount);
    CHECK(packed_create_info->enabledExtensionCount == expected.create_info.enabledExtensionCount);

    REQUIRE(packed_create_info->pApplicationInfo != nullptr);
    const auto application_info_offset = create_info_offset + encoded_offset(packed_create_info->pApplicationInfo);
    const VkApplicationInfo * packed_application_info = nullptr;
    REQUIRE(::vkfwd::generated::structure::unpack_VkApplicationInfo(request_blob, application_info_offset, &packed_application_info) == VK_SUCCESS);
    REQUIRE(packed_application_info != nullptr);
    CHECK(packed_application_info->pNext == nullptr);
    CHECK(packed_application_info->applicationVersion == expected.application_info.applicationVersion);
    CHECK(packed_application_info->engineVersion == expected.application_info.engineVersion);
    CHECK(packed_application_info->apiVersion == expected.application_info.apiVersion);
    check_relative_string(request_blob, application_info_offset, packed_application_info->pApplicationName, expected.application_info.pApplicationName);
    check_relative_string(request_blob, application_info_offset, packed_application_info->pEngineName, expected.application_info.pEngineName);
    check_relative_string_array(request_blob, create_info_offset, packed_create_info->ppEnabledLayerNames,
                                {{"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"}});
    check_relative_string_array(request_blob, create_info_offset, packed_create_info->ppEnabledExtensionNames,
                                {{"VK_EXT_debug_utils", "VK_KHR_surface"}});

    REQUIRE(actual.pAllocator != nullptr);
    const auto & packed_allocator = object_at<VkAllocationCallbacks>(request_blob, encoded_offset(actual.pAllocator));
    check_allocator_callbacks(packed_allocator, expected.allocator);

    Blob response_blob;
    Command::Response response {{.return_value = expected.response_result, .pInstance = &expected.response_instance}};
    Command::ResponsePacket response_packet;
    REQUIRE(Command::pack_response(response_blob, response, response_packet) == VK_SUCCESS);
    return response_blob;
}}

}} // namespace

TEST_CASE("vkCreateInstance generated forwarder round trips packed parameters and response") {{
    auto & expected = scenario();
    install_pack_unpack_channel(handle_flush);

    VkInstance instance = VK_NULL_HANDLE;
    expected.output_instance = &instance;
    const VkResult result = vkfwd::forwarder::generated::vkCreateInstance(&expected.create_info, &expected.allocator, &instance);

    CHECK(channel_state().processed);
    CHECK(result == expected.response_result);
    CHECK(instance == expected.response_instance);
}}

}} // namespace vkfwd::forwarder::generated::test
"""


def vkcreatedevice_test_content(metadata: dict[str, object]) -> str:
    return f"""#include "support.hpp"

#include "generated/command/vkCreateDevice.hpp"
#include "generated/dispatch_table.hpp"
#include "generated/structure/core.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::forwarder::generated::test {{
namespace {{

using Command = ::vkfwd::generated::commands::vkCreateDevice::Command;

struct Scenario {{
    int                              allocator_user_data = 0x42;
    VkAllocationCallbacks            allocator;
    VkPhysicalDevice                 physical_device = test_handle<VkPhysicalDevice>(0x202);
    std::array<float, 2>             queue_priorities;
    VkDeviceQueueCreateInfo          queue_create_info;
    std::array<const char *, 1>       layers;
    std::array<const char *, 2>       extensions;
    VkPhysicalDeviceFeatures         enabled_features;
    VkDeviceCreateInfo               create_info;
    VkDevice *                       output_device = nullptr;
    VkDevice                         response_device;
    VkResult                         response_result = VK_NOT_READY;
}};

Scenario make_scenario() {{
    Scenario scenario;
    scenario.allocator        = test_allocator(&scenario.allocator_user_data);
    scenario.queue_priorities = {{0.25f, 0.75f}};
    scenario.queue_create_info = VkDeviceQueueCreateInfo {{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VkDeviceQueueCreateFlags {{0x2}},
        .queueFamilyIndex = 3,
        .queueCount       = static_cast<std::uint32_t>(scenario.queue_priorities.size()),
        .pQueuePriorities = scenario.queue_priorities.data(),
    }};
    scenario.layers                      = {{"VK_LAYER_VKFWD_device"}};
    scenario.extensions                  = {{"VK_KHR_swapchain", "VK_EXT_private_data"}};
    scenario.enabled_features            = VkPhysicalDeviceFeatures {{}};
    scenario.enabled_features.robustBufferAccess = VK_TRUE;
    scenario.enabled_features.samplerAnisotropy  = VK_TRUE;
    scenario.create_info = VkDeviceCreateInfo {{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkDeviceCreateFlags {{0x8}},
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &scenario.queue_create_info,
        .enabledLayerCount       = static_cast<std::uint32_t>(scenario.layers.size()),
        .ppEnabledLayerNames     = scenario.layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(scenario.extensions.size()),
        .ppEnabledExtensionNames = scenario.extensions.data(),
        .pEnabledFeatures        = &scenario.enabled_features,
    }};
    scenario.response_device = test_handle<VkDevice>(0x303);
    return scenario;
}}

Scenario & scenario() {{
    static Scenario value = make_scenario();
    return value;
}}

Blob handle_flush(Blob & request_blob) {{
    auto & expected = scenario();
    const auto packet = first_command_chunk(request_blob);

    Command::Parameters actual;
    REQUIRE(Command::unpack_parameters(request_blob, packet, actual) == VK_SUCCESS);
    CHECK(actual.physicalDevice == expected.physical_device);
    CHECK(actual.pDevice == expected.output_device);

    REQUIRE(actual.pCreateInfo != nullptr);
    const auto create_info_offset = encoded_offset(actual.pCreateInfo);
    const VkDeviceCreateInfo * packed_create_info = nullptr;
    REQUIRE(::vkfwd::generated::structure::unpack_VkDeviceCreateInfo(request_blob, create_info_offset, &packed_create_info) == VK_SUCCESS);
    REQUIRE(packed_create_info != nullptr);
    CHECK(packed_create_info->pNext == nullptr);
    CHECK(packed_create_info->flags == expected.create_info.flags);
    CHECK(packed_create_info->queueCreateInfoCount == expected.create_info.queueCreateInfoCount);
    CHECK(packed_create_info->enabledLayerCount == expected.create_info.enabledLayerCount);
    CHECK(packed_create_info->enabledExtensionCount == expected.create_info.enabledExtensionCount);

    REQUIRE(packed_create_info->pQueueCreateInfos != nullptr);
    const auto queue_info_offset = create_info_offset + encoded_offset(packed_create_info->pQueueCreateInfos);
    const VkDeviceQueueCreateInfo * packed_queue_info = nullptr;
    REQUIRE(::vkfwd::generated::structure::unpack_VkDeviceQueueCreateInfo(request_blob, queue_info_offset, &packed_queue_info) == VK_SUCCESS);
    REQUIRE(packed_queue_info != nullptr);
    CHECK(packed_queue_info->pNext == nullptr);
    CHECK(packed_queue_info->flags == expected.queue_create_info.flags);
    CHECK(packed_queue_info->queueFamilyIndex == expected.queue_create_info.queueFamilyIndex);
    CHECK(packed_queue_info->queueCount == expected.queue_create_info.queueCount);

    REQUIRE(packed_queue_info->pQueuePriorities != nullptr);
    const auto priorities_offset = queue_info_offset + encoded_offset(packed_queue_info->pQueuePriorities);
    const auto priorities_view = request_blob.data_at(priorities_offset, expected.queue_priorities.size() * sizeof(float));
    REQUIRE(priorities_view.data() != nullptr);
    const auto * priorities = reinterpret_cast<const float *>(priorities_view.data());
    CHECK(priorities[0] == expected.queue_priorities[0]);
    CHECK(priorities[1] == expected.queue_priorities[1]);

    check_relative_string_array(request_blob, create_info_offset, packed_create_info->ppEnabledLayerNames, {{"VK_LAYER_VKFWD_device"}});
    check_relative_string_array(request_blob, create_info_offset, packed_create_info->ppEnabledExtensionNames,
                                {{"VK_KHR_swapchain", "VK_EXT_private_data"}});

    REQUIRE(packed_create_info->pEnabledFeatures != nullptr);
    const auto features_offset = create_info_offset + encoded_offset(packed_create_info->pEnabledFeatures);
    const auto & packed_features = object_at<VkPhysicalDeviceFeatures>(request_blob, features_offset);
    CHECK(packed_features.robustBufferAccess == VK_TRUE);
    CHECK(packed_features.samplerAnisotropy == VK_TRUE);

    REQUIRE(actual.pAllocator != nullptr);
    const auto & packed_allocator = object_at<VkAllocationCallbacks>(request_blob, encoded_offset(actual.pAllocator));
    check_allocator_callbacks(packed_allocator, expected.allocator);

    Blob response_blob;
    Command::Response response {{.return_value = expected.response_result, .pDevice = &expected.response_device}};
    Command::ResponsePacket response_packet;
    REQUIRE(Command::pack_response(response_blob, response, response_packet) == VK_SUCCESS);
    return response_blob;
}}

}} // namespace

TEST_CASE("vkCreateDevice generated forwarder round trips packed parameters and response") {{
    auto & expected = scenario();
    install_pack_unpack_channel(handle_flush);

    VkDevice device = VK_NULL_HANDLE;
    expected.output_device = &device;
    const VkResult result = vkfwd::forwarder::generated::vkCreateDevice(expected.physical_device, &expected.create_info, &expected.allocator, &device);

    CHECK(channel_state().processed);
    CHECK(result == expected.response_result);
    CHECK(device == expected.response_device);
}}

}} // namespace vkfwd::forwarder::generated::test
"""


def destroy_test_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    namespace = command_namespace(str(command["name"]))
    enum_name = command_enum_name(str(command["name"]))
    dispatch_name = str(command["parameters"][0]["name"])
    handle_type = str(command["parameters"][0]["type"])
    handle_value = "0x404" if command["name"] == "vkDestroyInstance" else "0x505"
    return f"""#include "support.hpp"

#include "generated/command/{command['name']}.hpp"
#include "generated/dispatch_table.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::generated::test {{
namespace {{

using Command = ::vkfwd::generated::commands::{namespace}::Command;

struct Scenario {{
    int                   allocator_user_data = {handle_value};
    VkAllocationCallbacks allocator;
    {handle_type}         {dispatch_name} = test_handle<{handle_type}>({handle_value});
}};

Scenario make_scenario() {{
    Scenario scenario;
    scenario.allocator = test_allocator(&scenario.allocator_user_data);
    return scenario;
}}

Scenario & scenario() {{
    static Scenario value = make_scenario();
    return value;
}}

Blob handle_flush(Blob & request_blob) {{
    auto & expected = scenario();
    const auto packet = first_command_chunk(request_blob);

    Command::Parameters actual;
    REQUIRE(Command::unpack_parameters(request_blob, packet, actual) == VK_SUCCESS);
    CHECK(actual.{dispatch_name} == expected.{dispatch_name});
    CHECK(actual.pAllocator == &expected.allocator);
    check_allocator_callbacks(*actual.pAllocator, expected.allocator);

    // Deferrable generated commands acknowledge successful channel processing
    // with an empty response blob; there is no response packet to unpack.
    return Blob {{}};
}}

}} // namespace

TEST_CASE("{command['name']} generated forwarder packs parameters when flushed") {{
    auto & expected = scenario();
    install_pack_unpack_channel(handle_flush);

    vkfwd::forwarder::generated::{command['name']}(expected.{dispatch_name}, &expected.allocator);
    Blob response_blob = Forwarder::instance().flush();

    CHECK(channel_state().processed);
    CHECK(response_blob.size() == 0);
}}

}} // namespace vkfwd::forwarder::generated::test
"""


def forwarder_test_content(
    metadata: dict[str, object], command: dict[str, object]
) -> str:
    if command["name"] == "vkCreateInstance":
        return vkcreateinstance_test_content(metadata)
    if command["name"] == "vkCreateDevice":
        return vkcreatedevice_test_content(metadata)
    return destroy_test_content(metadata, command)


def write_forwarder_test_files(
    metadata: dict[str, object], forwarder_dir: Path
) -> None:
    test_dir = forwarder_dir / "test"
    test_dir.mkdir(parents=True, exist_ok=True)
    (test_dir / "support.hpp").write_text(
        forwarder_test_support_content(metadata), encoding="utf-8"
    )
    local_sources = []
    for command in metadata["commands"]:
        file_name = f"{command['name']}_test.cpp"
        local_sources.append(file_name)
        (test_dir / file_name).write_text(
            forwarder_test_content(metadata, command), encoding="utf-8"
        )

    manifest_sources = "\n".join(f"  {source}" for source in local_sources)
    (test_dir / "internal-test.cmake").write_text(
        f"""# This generated manifest is consumed by dev/test/internal-test/CMakeLists.txt.
# Keep entries relative so the generated forwarder tests remain self-contained.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
{manifest_sources})
""",
        encoding="utf-8",
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

template<class T>
const T & object_at(const Blob & blob, std::size_t offset) {{
    const auto view = blob.data_at(offset, sizeof(T));
    REQUIRE(view.data() != nullptr);
    return *reinterpret_cast<const T *>(view.data());
}}

inline void check_relative_string(const Blob & blob, std::size_t base_offset, const char * encoded_value, std::string_view expected) {{
    REQUIRE(encoded_value != nullptr);
    const std::size_t string_offset = base_offset + encoded_offset(encoded_value);
    const auto        view          = blob.data_at(string_offset, expected.size() + 1);
    REQUIRE(view.data() != nullptr);
    const auto * value = reinterpret_cast<const char *>(view.data());
    CHECK(std::string_view(value, expected.size()) == expected);
    CHECK(value[expected.size()] == '\\0');
}}

inline void check_relative_string_array(const Blob & blob, std::size_t base_offset, const char * const * encoded_values,
                                        std::initializer_list<std::string_view> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    const std::size_t array_offset = base_offset + encoded_offset(encoded_values);
    const auto        slots_view   = blob.data_at(array_offset, expected.size() * sizeof(std::uintptr_t));
    REQUIRE(slots_view.data() != nullptr);
    const auto * slots = reinterpret_cast<const std::uintptr_t *>(slots_view.data());

    std::size_t index = 0;
    for (std::string_view expected_value : expected) {{
        REQUIRE(slots[index] != 0);
        const auto string_view = blob.data_at(base_offset + static_cast<std::size_t>(slots[index]), expected_value.size() + 1);
        REQUIRE(string_view.data() != nullptr);
        const auto * actual_value = reinterpret_cast<const char *>(string_view.data());
        CHECK(std::string_view(actual_value, expected_value.size()) == expected_value);
        CHECK(actual_value[expected_value.size()] == '\\0');
        ++index;
    }}
}}

template<class T>
void check_relative_plain_array(const Blob & blob, std::size_t base_offset, const T * encoded_values, std::initializer_list<T> expected) {{
    if (expected.size() == 0) {{
        CHECK(encoded_values == nullptr);
        return;
    }}

    REQUIRE(encoded_values != nullptr);
    const std::size_t array_offset = base_offset + encoded_offset(encoded_values);
    const auto        view         = blob.data_at(array_offset, expected.size() * sizeof(T));
    REQUIRE(view.data() != nullptr);
    const auto * actual_values = reinterpret_cast<const T *>(view.data());

    std::size_t index = 0;
    for (const T & expected_value : expected) {{
        CHECK(actual_values[index] == expected_value);
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
    REQUIRE(unpack_VkApplicationInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->applicationVersion == value.applicationVersion);
    CHECK(actual->engineVersion == value.engineVersion);
    CHECK(actual->apiVersion == value.apiVersion);
    check_relative_string(blob, packed.offset, actual->pApplicationName, value.pApplicationName);
    check_relative_string(blob, packed.offset, actual->pEngineName, value.pEngineName);
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
    REQUIRE(unpack_VkInstanceCreateInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->enabledLayerCount == value.enabledLayerCount);
    CHECK(actual->enabledExtensionCount == value.enabledExtensionCount);
    check_relative_string_array(blob, packed.offset, actual->ppEnabledLayerNames, {{"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"}});
    check_relative_string_array(blob, packed.offset, actual->ppEnabledExtensionNames, {{"VK_EXT_debug_utils", "VK_KHR_surface"}});

    REQUIRE(actual->pApplicationInfo != nullptr);
    const auto app_offset = packed.offset + encoded_offset(actual->pApplicationInfo);
    const VkApplicationInfo * actual_app = nullptr;
    REQUIRE(unpack_VkApplicationInfo(blob, app_offset, &actual_app) == VK_SUCCESS);
    REQUIRE(actual_app != nullptr);
    CHECK(actual_app->applicationVersion == app.applicationVersion);
    CHECK(actual_app->engineVersion == app.engineVersion);
    check_relative_string(blob, app_offset, actual_app->pApplicationName, app.pApplicationName);
    check_relative_string(blob, app_offset, actual_app->pEngineName, app.pEngineName);
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
    REQUIRE(unpack_VkDeviceQueueCreateInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->queueFamilyIndex == value.queueFamilyIndex);
    CHECK(actual->queueCount == value.queueCount);
    check_relative_plain_array(blob, packed.offset, actual->pQueuePriorities, {{0.25f, 0.75f}});
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
    REQUIRE(unpack_VkDeviceCreateInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->flags == value.flags);
    CHECK(actual->queueCreateInfoCount == value.queueCreateInfoCount);
    CHECK(actual->enabledLayerCount == value.enabledLayerCount);
    CHECK(actual->enabledExtensionCount == value.enabledExtensionCount);
    check_relative_string_array(blob, packed.offset, actual->ppEnabledLayerNames, {{"VK_LAYER_VKFWD_device"}});
    check_relative_string_array(blob, packed.offset, actual->ppEnabledExtensionNames, {{"VK_KHR_swapchain", "VK_EXT_private_data"}});

    REQUIRE(actual->pQueueCreateInfos != nullptr);
    const auto queue_offset = packed.offset + encoded_offset(actual->pQueueCreateInfos);
    const VkDeviceQueueCreateInfo * actual_queue = nullptr;
    REQUIRE(unpack_VkDeviceQueueCreateInfo(blob, queue_offset, &actual_queue) == VK_SUCCESS);
    REQUIRE(actual_queue != nullptr);
    CHECK(actual_queue->queueFamilyIndex == queue.queueFamilyIndex);
    CHECK(actual_queue->queueCount == queue.queueCount);
    check_relative_plain_array(blob, queue_offset, actual_queue->pQueuePriorities, {{0.5f, 1.0f}});

    REQUIRE(actual->pEnabledFeatures != nullptr);
    const auto & actual_features = object_at<VkPhysicalDeviceFeatures>(blob, packed.offset + encoded_offset(actual->pEnabledFeatures));
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
    REQUIRE(unpack_VkDeviceGroupDeviceCreateInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

    CHECK(actual->sType == value.sType);
    CHECK(actual->pNext == nullptr);
    CHECK(actual->physicalDeviceCount == value.physicalDeviceCount);
    check_relative_plain_array(blob, packed.offset, actual->pPhysicalDevices,
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
    REQUIRE(unpack_VkDeviceQueueGlobalPriorityCreateInfo(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);

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
    REQUIRE(unpack_VkPhysicalDeviceFeatures2(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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
    REQUIRE(unpack_VkPhysicalDeviceVulkan11Features(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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
    REQUIRE(unpack_VkPhysicalDeviceVulkan12Features(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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
    REQUIRE(unpack_VkPhysicalDeviceVulkan13Features(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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
    REQUIRE(unpack_VkPhysicalDeviceVulkan14Features(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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
    REQUIRE(unpack_VkPhysicalDeviceDescriptorIndexingFeatures(blob, packed.offset, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
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


def format_generated_files(output_dir: Path, forwarder_output_dir: Path) -> None:
    root_dir = repo_root()
    root_resolved = root_dir.resolve()
    scopes = []
    for output_path in (output_dir, forwarder_output_dir):
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


def generate(output_dir: Path, forwarder_output_dir: Path) -> None:
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
    write_structure_test_files(metadata, output_dir)
    write_forwarder_files(metadata, forwarder_output_dir)
    write_forwarder_test_files(metadata, forwarder_output_dir)
    format_generated_files(output_dir, forwarder_output_dir)


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
    args = parser.parse_args()
    generate(args.output_dir, args.forwarder_output_dir)


if __name__ == "__main__":
    main()
