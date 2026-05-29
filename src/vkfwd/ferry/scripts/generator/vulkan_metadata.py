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
GENERATOR_SCHEMA_VERSION = 1
WIRE_MAJOR = 1
WIRE_MINOR = 0
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
    # Command IDs are part of the wire contract, so they must not depend on
    # registry ordering. The salt fixes this scheme for compatible protocol
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
        "<!-- Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand. -->",
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

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
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
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    fields = "\n".join(
        f"  {parameter_cxx_type(parameter)} {parameter['name']} = {{}};"
        for parameter in command["parameters"]
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"
#include "generated/vulkan_manual_hooks.hpp"

namespace vkfwd::generated::commands::{namespace} {{

struct Parameters {{
{fields}
}};

struct Response {{
{response_return_member(command)}{response_output_fields(command)}
}};

struct ResponsePacket;

struct ParameterPacket {{
  CommandId command_id = CommandId::{enum_name};
  Parameters parameters;
  using ResponsePacket = vkfwd::generated::commands::{namespace}::ResponsePacket;
}};

struct ResponsePacket {{
  CommandId command_id = CommandId::{enum_name};
  Response response;
}};

class Command {{
public:
  using Parameters = vkfwd::generated::commands::{namespace}::Parameters;
  using Response = vkfwd::generated::commands::{namespace}::Response;
  using ParameterPacket = vkfwd::generated::commands::{namespace}::ParameterPacket;
  using ResponsePacket = vkfwd::generated::commands::{namespace}::ResponsePacket;

  static VkResult pack_parameters(const Parameters& parameters,
                                  ParameterPacket* packet);
  static VkResult unpack_parameters(const ParameterPacket& packet,
                                    Parameters* parameters);
  static VkResult pack_response(const Response& response,
                                ResponsePacket* packet);
  static VkResult unpack_response(const ResponsePacket& packet,
                                  Response* response);
}};

}} // namespace vkfwd::generated::commands::{namespace}

#if __has_include("hook/{command['name']}Hook.hpp")
#include "hook/{command['name']}Hook.hpp"
#endif
"""


def command_source_content(metadata: dict[str, object], command: dict[str, object]) -> str:
    enum_name = command_enum_name(command["name"])
    namespace = command_namespace(command["name"])
    return f"""#include "generated/command/{command['name']}.hpp"

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::generated::commands::{namespace} {{

VkResult Command::pack_parameters(const Parameters& parameters,
                                  ParameterPacket* packet) {{
  if (!packet) {{
    return VK_ERROR_UNKNOWN;
  }}

  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_pack_enabled) {{
    Parameters hook_parameters = parameters;
    Hooks::before_pack(hook_parameters);

    // This generated slice captures the command shape and argument values but
    // intentionally does not claim wire-stable Vulkan replay yet. Pointer-bearing
    // parameters, arrays, and pNext chains must be deep-copied by later generated
    // serializers before a packet can outlive the source call safely.
    *packet = ParameterPacket{{CommandId::{enum_name}, hook_parameters}};

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack(*packet);
    }}
    return VK_SUCCESS;
  }} else {{
    // With hooks disabled, const-reference input avoids an avoidable pre-pack
    // copy; the packet copy is the ownership boundary for the captured call.
    *packet = ParameterPacket{{CommandId::{enum_name}, parameters}};

    if constexpr (Hooks::after_pack_enabled) {{
      Hooks::after_pack(*packet);
    }}
    return VK_SUCCESS;
  }}
}}

VkResult Command::unpack_parameters(const ParameterPacket& packet,
                                    Parameters* parameters) {{
  if (!parameters) {{
    return VK_ERROR_UNKNOWN;
  }}

  using Hooks = ::vkfwd::manual::CommandHooks<CommandId::{enum_name}>;
  if constexpr (Hooks::before_unpack_enabled) {{
    Hooks::before_unpack(packet);
  }}

  *parameters = packet.parameters;

  if constexpr (Hooks::after_unpack_enabled) {{
    Hooks::after_unpack(*parameters);
  }}
  return VK_SUCCESS;
}}

VkResult Command::pack_response(const Response& response,
                                ResponsePacket* packet) {{
  if (!packet) {{
    return VK_ERROR_UNKNOWN;
  }}
  *packet = ResponsePacket{{CommandId::{enum_name}, response}};
  return VK_SUCCESS;
}}

VkResult Command::unpack_response(const ResponsePacket& packet,
                                  Response* response) {{
  if (!response) {{
    return VK_ERROR_UNKNOWN;
  }}
  *response = packet.response;
  return VK_SUCCESS;
}}

}} // namespace vkfwd::generated::commands::{namespace}
"""


def write_command_files(metadata: dict[str, object], output_dir: Path) -> None:
    commands_dir = output_dir / "command"
    commands_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "README.md").write_text(
        """# Generated Vulkan Code

Every file in this directory tree is produced by
`src/vkfwd/ferry/scripts/generator/vulkan_metadata.py`. Do not place manual code here; regeneration
may replace these files without preserving local edits.

Per-command generated code and per-command generated metadata live under
`command/`. Human-written hook code belongs under
`src/vkfwd/ferry/core/hook/<api>Hook.hpp` and optional matching `.cpp` files.
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


def write_cxx_header(metadata: dict[str, object], path: Path) -> None:
    commands = metadata["commands"]
    enum_values = "\n".join(
        f"  {command_enum_name(command['name'])} = {command['id']},"
        for command in commands
    )
    content = f"""#pragma once

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "protocol.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace vkfwd::generated {{

enum class CommandId : std::uint32_t {{
{enum_values}
}};

enum class CommandLevel {{
  global,
  instance,
  device,
}};

constexpr WireVersion kGeneratedWireVersion{{
    {metadata['protocol']['wire_major']},
    {metadata['protocol']['wire_minor']}}};

constexpr VulkanApiVersion kGeneratedVulkanApiVersion{{
    {metadata['versions']['vulkan_api']['major']},
    {metadata['versions']['vulkan_api']['minor']},
    {metadata['versions']['vulkan_api']['patch']}}};

constexpr std::uint32_t kGeneratedSchemaVersion =
    {metadata['protocol']['generator_schema_version']};

constexpr Handshake current_handshake() {{
  return Handshake{{
      kStreamMagic,
      kGeneratedWireVersion,
      kGeneratedVulkanApiVersion,
      kGeneratedSchemaVersion}};
}}

struct CommandInfo {{
  CommandId id;
  std::string_view name;
  CommandLevel level;
  std::string_view return_type;
  std::string_view dispatch_parameter;
  std::span<const std::string_view> creates_handles;
}};

struct InstanceDispatchTable {{
  // This generated slice only loads instance-level creation commands. The
  // owning layer must still preserve loader-chain lifetime rules and map
  // child dispatchable handles, such as VkPhysicalDevice, back to the instance
  // table before it can invoke vkCreateDevice through this table.
  PFN_vkCreateDevice create_device = nullptr;
}};

std::span<const CommandInfo> command_infos();
const CommandInfo* find_command(std::string_view name);
bool is_generated_command(std::string_view name);
InstanceDispatchTable load_instance_dispatch_table(
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    VkInstance instance);

}} // namespace vkfwd::generated
"""
    path.write_text(content, encoding="utf-8")


def write_cxx_source(metadata: dict[str, object], path: Path) -> None:
    commands = metadata["commands"]
    arrays: list[str] = []
    infos: list[str] = []
    for command in commands:
        enum_name = command_enum_name(command["name"])
        arrays.append(
            f"constexpr std::array<std::string_view, {len(command['creates_handles'])}> "
            f"k{enum_name}CreatesHandles = {cxx_string_list(command['creates_handles'])};"
        )
        level = command["level"]
        dispatch_parameter = command["dispatch_parameter"] or ""
        infos.append(
            "  {\n"
            f"    CommandId::{enum_name},\n"
            f"    \"{command['name']}\",\n"
            f"    CommandLevel::{level},\n"
            f"    \"{command['return_type']}\",\n"
            f"    \"{dispatch_parameter}\",\n"
            f"    k{enum_name}CreatesHandles,\n"
            "  },"
        )
    content = f"""#include "generated/vulkan_api.hpp"

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <array>

namespace vkfwd::generated {{
namespace {{

{chr(10).join(arrays)}

constexpr std::array<CommandInfo, {len(commands)}> kCommandInfos = {{{{
{chr(10).join(infos)}
}}}};

}} // namespace

std::span<const CommandInfo> command_infos() {{
  return kCommandInfos;
}}

const CommandInfo* find_command(std::string_view name) {{
  for (const auto& command : kCommandInfos) {{
    if (command.name == name) {{
      return &command;
    }}
  }}
  return nullptr;
}}

bool is_generated_command(std::string_view name) {{
  return find_command(name) != nullptr;
}}

InstanceDispatchTable load_instance_dispatch_table(
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    VkInstance instance) {{
  InstanceDispatchTable table;
  if (!get_instance_proc_addr) {{
    return table;
  }}

  // Generated dispatch loading must use the next layer's lookup function, not
  // the loader's public entry point, or interception can accidentally recurse
  // into vkfwd instead of preserving the loader chain.
  table.create_device = reinterpret_cast<PFN_vkCreateDevice>(
      get_instance_proc_addr(instance, "vkCreateDevice"));
  return table;
}}

}} // namespace vkfwd::generated
"""
    path.write_text(content, encoding="utf-8")


def forwarder_hooks_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
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

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
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
    return f"""#include "generated/vulkan_forwarder.hpp"

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
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
    return f"""#include "generated/vulkan_forwarder.hpp"

#include "forwarder.hpp"
#include "generated/command/{command['name']}.hpp"
#include "generated/vulkan_forwarder_hooks.hpp"

// Generated by src/vkfwd/ferry/scripts/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include <algorithm>

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

  Command::Parameters parameters{parameter_initializer_list(command)};
  Command::ParameterPacket request;
  VkResult status = Command::pack_parameters(parameters, &request);
  if (status != VK_SUCCESS) {{
{failure_return}
  }}

  Command::ResponsePacket placeholder_response;
  status = Command::pack_response({response_initializer(command)},
                                  &placeholder_response);
  if (status != VK_SUCCESS) {{
{failure_return}
  }}

  Command::ResponsePacket response_packet;
  status = ::vkfwd::Forwarder::instance().forward(
      "{command['name']}", request, placeholder_response, &response_packet);
  if (status != VK_SUCCESS) {{
{failure_return}
  }}

  Command::Response response;
  status = Command::unpack_response(response_packet, &response);
  if (status != VK_SUCCESS) {{
{failure_return}
  }}

  if constexpr (Hooks::after_response_unpack_enabled) {{
    Hooks::after_response_unpack(response);
  }}
{output_assignments}
  // The endpoint response currently uses a generated placeholder because the
  // transport contract has not grown real response bytes yet. Once endpoints
  // carry return payloads, generated code should return that unpacked value
  // without adding source-side validation or local Vulkan state.
{return_statement}
}}

}} // namespace vkfwd::forwarder::generated
"""


def write_forwarder_files(metadata: dict[str, object], forwarder_dir: Path) -> None:
    commands_dir = forwarder_dir / "command"
    commands_dir.mkdir(parents=True, exist_ok=True)
    (forwarder_dir / "vulkan_forwarder.hpp").write_text(
        forwarder_header_content(metadata), encoding="utf-8"
    )
    (forwarder_dir / "vulkan_forwarder.cpp").write_text(
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
            "wire_major": WIRE_MAJOR,
            "wire_minor": WIRE_MINOR,
            "generator_schema_version": GENERATOR_SCHEMA_VERSION,
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
    write_cxx_header(metadata, output_dir / "vulkan_api.hpp")
    write_cxx_source(metadata, output_dir / "vulkan_api.cpp")
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
