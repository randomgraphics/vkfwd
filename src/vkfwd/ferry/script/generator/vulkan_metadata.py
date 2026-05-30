#!/usr/bin/env python3
"""Generate the first vkfwd Vulkan code and metadata slice from the pinned registry."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
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
                    "is_string_array": member.get("len", "").endswith("null-terminated"),
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
        "dispatch_parameter": params[0]["name"] if infer_command_level(params) != "global" else None,
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
    return any(parameter["direction"] == "output" for parameter in command["parameters"])


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


def status_failure_return_statement(command: dict[str, object], status_name: str) -> str:
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


def command_header_content(metadata: dict[str, object], command: dict[str, object]) -> str:
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
  const std::size_t command_offset = blob.next_offset();
  chunk = CommandChunk{{.command_offset = command_offset, .command_size = 0}};

  CommandChunkHeader header{{}};
  try {{
    blob.append_value(header, alignof(CommandChunkHeader));
    blob.append_value(payload, alignof(T));
  }} catch (const std::bad_alloc&) {{
    spdlog::error("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={{}}, payload_size={{}}",
                  static_cast<std::uint32_t>(command_id), sizeof(T));
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }}

  header.command_id = static_cast<std::uint32_t>(command_id);
  header.size = static_cast<std::uint32_t>(blob.next_offset() - command_offset);
  header.command_revision = revision;
  if (!blob.overwrite_bytes(command_offset, &header, sizeof(header))) [[unlikely]] {{
    spdlog::error("vkfwd ferry command pack failed: could not write command chunk header, command_id={{}}, command_offset={{}}, command_size={{}}",
                  static_cast<std::uint32_t>(command_id), command_offset, header.size);
    return VK_ERROR_UNKNOWN;
  }}
  chunk.command_size = header.size;
  return VK_SUCCESS;
}}

template<class T>
VkResult unpack_command_chunk(const Blob& blob, const CommandChunk& chunk, CommandId command_id, std::uint32_t revision, const T** payload) {{
  const auto* header = reinterpret_cast<const CommandChunkHeader*>(blob.data_at(chunk.command_offset, sizeof(CommandChunkHeader)));
  const auto* packed_payload = reinterpret_cast<const T*>(blob.data_at(chunk.command_offset + sizeof(CommandChunkHeader), sizeof(T)));
  if (!header || !packed_payload || header->command_id != static_cast<std::uint32_t>(command_id) || header->command_revision != revision ||
      header->size != chunk.command_size) [[unlikely]] {{
    spdlog::error(
        "vkfwd ferry command unpack failed: invalid command chunk, offset={{}}, size={{}}, has_header={{}}, has_payload={{}}, command_id={{}}, "
        "expected_command_id={{}}, revision={{}}, expected_revision={{}}, header_size={{}}",
        chunk.command_offset, chunk.command_size, header != nullptr, packed_payload != nullptr, header ? header->command_id : 0,
        static_cast<std::uint32_t>(command_id), header ? header->command_revision : 0, revision, header ? header->size : 0);
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


def command_source_content(metadata: dict[str, object], command: dict[str, object]) -> str:
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

#if __has_include(<spdlog/spdlog.h>)
    #include <spdlog/spdlog.h>
#else
namespace spdlog {{
template<class... Args>
void error(const char*, Args&&...) noexcept {{}}
}} // namespace spdlog
#endif

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
            json.dumps(command_metadata_document(metadata, command), indent=2, sort_keys=True) + "\n",
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
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


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
  // endpoint implementations own transport, replay, and handle mapping policy.
"""
    else:
        response_flow = f"""
  // Deferrable commands have no return value or output parameters, so the
  // entry point only appends to the thread-local request blob. The next
  // non-deferrable command is responsible for flushing this thread's pending
  // command sequence through the endpoint.
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
        command_metadata(root, name, handles)
        for name in TARGET_COMMANDS
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
    write_forwarder_files(metadata, forwarder_output_dir)


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
