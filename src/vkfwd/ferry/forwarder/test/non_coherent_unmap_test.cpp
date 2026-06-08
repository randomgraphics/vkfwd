#include "support.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/wire_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::forwarder::test {
namespace {

using ::vkfwd::kMemoryMapManagerRevision;
using ::vkfwd::memory_map::wire::MemoryMapResponse;
using ::vkfwd::memory_map::wire::MemoryUnmapRequestHeader;

// Shared scenario state. The static-storage flush handlers cannot capture
// (install_pack_unpack_transport takes a plain function pointer), so per-test
// expectations and assertions go through this singleton.
struct Scenario {
    VkDevice              device                   = test_handle<VkDevice>(0xB001);
    VkDeviceMemory        memory                   = test_handle<VkDeviceMemory>(0xC001);
    VkMemoryPropertyFlags property_flags           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    std::uint32_t         memory_type_index        = 3;
    VkDeviceSize          allocation_size          = 64 * 1024;
    VkDeviceSize          non_coherent_atom_size   = 64;
    std::size_t           min_memory_map_alignment = 4096;

    // Map-phase parameters; reused so the unmap handler can compare against
    // the offset/size the receiver-side mapping was actually built with.
    VkDeviceSize     map_offset = 256;
    VkDeviceSize     map_size   = 4096;
    VkMemoryMapFlags map_flags  = 0;

    // Call-order state. The map and unmap entries share a single transport
    // handler (install_pack_unpack_transport replaces the previous one, so a
    // single map-then-unmap sequence needs a stateful dispatcher).
    int call_count = 0;

    // Map-phase observations.
    bool          saw_map_request    = false;
    std::uint32_t saw_map_command_id = 0;

    // Unmap-phase observations.
    bool                     saw_unmap_request    = false;
    std::uint32_t            saw_unmap_command_id = 0;
    std::uint32_t            saw_unmap_revision   = 0;
    std::size_t              saw_unmap_chunk_size = 0;
    MemoryUnmapRequestHeader saw_unmap_payload {};
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

void record_scenario_allocation(const Scenario & s) {
    auto & forwarder_manager = ::vkfwd::MemoryMapForwarder::instance();
    forwarder_manager.forget_allocation(s.memory);
    forwarder_manager.record_allocation(s.device, s.memory, s.property_flags, s.memory_type_index, s.allocation_size, s.non_coherent_atom_size,
                                        s.min_memory_map_alignment);
}

// Synthesizes the map-phase response so the forwarder map() body succeeds.
CommandStream make_map_response_stream(const Scenario & s) {
    CommandStream     stream;
    auto              view = stream.grow<MemoryMapResponse>(1, alignof(MemoryMapResponse));
    MemoryMapResponse response {
        .manager_revision = kMemoryMapManagerRevision,
        .return_value     = static_cast<std::int32_t>(VK_SUCCESS),
        .effective_size   = s.map_size,
    };
    view.set(0, response);
    return stream;
}

// Stateful dispatcher: first call handles the map request, second handles the
// unmap. We branch on Scenario::call_count instead of installing two separate
// handlers because install_pack_unpack_transport() resets the request stream,
// which would discard the active mapping state between calls.
CommandStream handle_map_then_unmap(CommandStream & request_stream) {
    auto &      s      = scenario();
    const Range packet = first_command_range(request_stream);
    auto        bytes  = command_view(request_stream, packet);

    REQUIRE(bytes.size() >= sizeof(CommandChunkHeader));
    CommandChunkHeader header {};
    std::memcpy(&header, bytes.address(0), sizeof(header));

    if (s.call_count == 0) {
        // First call: map. Just observe enough to confirm it ran; the body of
        // these tests is the unmap branch below.
        s.saw_map_request    = true;
        s.saw_map_command_id = header.command_id;
        ++s.call_count;
        return make_map_response_stream(s);
    }

    // Subsequent call: unmap. Record the entire header + payload so the test
    // body can assert chunk shape, command id, range_count, and the carried
    // mapped_offset / mapped_size.
    s.saw_unmap_request    = true;
    s.saw_unmap_command_id = header.command_id;
    s.saw_unmap_revision   = header.command_revision;
    s.saw_unmap_chunk_size = header.size;

    constexpr std::size_t kPayloadOffset = (sizeof(CommandChunkHeader) + alignof(MemoryUnmapRequestHeader) - 1) & ~(alignof(MemoryUnmapRequestHeader) - 1);
    REQUIRE(bytes.size() >= kPayloadOffset + sizeof(MemoryUnmapRequestHeader));
    std::memcpy(&s.saw_unmap_payload, bytes.address(kPayloadOffset), sizeof(MemoryUnmapRequestHeader));

    ++s.call_count;
    // N2 unmap response is empty by design; the forwarder ignores its bytes.
    return CommandStream {};
}

} // namespace

TEST_CASE("NonCoherentForwarderAllocation::unmap sends a header-only MemoryUnmap chunk") {
    auto & s = scenario();
    s        = Scenario {};

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_then_unmap);

    // Map first so unmap has real reservation/offset/size state to send.
    void *         mapped     = nullptr;
    const VkResult map_result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.map_offset, s.map_size, s.map_flags, &mapped);
    REQUIRE(map_result == VK_SUCCESS);
    REQUIRE(mapped != nullptr);
    REQUIRE(s.saw_map_request);

    ::vkfwd::forwarder::generated::vkUnmapMemory_entry(s.device, s.memory);

    REQUIRE(s.saw_unmap_request);
    CHECK(s.saw_unmap_command_id == static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryUnmap));
    CHECK(s.saw_unmap_revision == kMemoryMapManagerRevision);
    // range_count == 0 is the N2 invariant; phase 3a coherent unmap will set
    // this to 1, which is the marker that future test code must update.
    CHECK(s.saw_unmap_payload.range_count == 0);
    // Header-only chunk: header + 48-byte MemoryUnmapRequestHeader, no
    // MemoryTransferRange entries and no payload bytes following.
    constexpr std::size_t kPayloadOffset = (sizeof(CommandChunkHeader) + alignof(MemoryUnmapRequestHeader) - 1) & ~(alignof(MemoryUnmapRequestHeader) - 1);
    CHECK(s.saw_unmap_chunk_size == kPayloadOffset + sizeof(MemoryUnmapRequestHeader));
    CHECK(s.saw_unmap_payload.manager_revision == kMemoryMapManagerRevision);
    CHECK(s.saw_unmap_payload.device == s.device);
    CHECK(s.saw_unmap_payload.memory == s.memory);
    CHECK(s.saw_unmap_payload.mapped_offset == s.map_offset);
    CHECK(s.saw_unmap_payload.mapped_size == s.map_size);

    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

TEST_CASE("NonCoherentForwarderAllocation::unmap leaves the allocation recorded so vkFreeMemory still owns its lifetime") {
    auto & s = scenario();
    s        = Scenario {};

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_then_unmap);

    void *         mapped     = nullptr;
    const VkResult map_result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.map_offset, s.map_size, s.map_flags, &mapped);
    REQUIRE(map_result == VK_SUCCESS);
    REQUIRE(mapped != nullptr);

    ::vkfwd::forwarder::generated::vkUnmapMemory_entry(s.device, s.memory);
    REQUIRE(s.saw_unmap_request);

    // Recording invariant: unmap releases the VA reservation but does not
    // delete the per-handle entry. vkFreeMemory is the only operation that
    // forgets the allocation; verifying the size is still queryable confirms
    // unmap did not erase the entry behind that contract.
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(s.memory) == s.allocation_size);

    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

TEST_CASE("NonCoherentForwarderAllocation::unmap allows a clean re-map afterward") {
    auto & s = scenario();
    s        = Scenario {};

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_then_unmap);

    // First map → unmap.
    void *         mapped_first = nullptr;
    const VkResult first_map_result =
        ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.map_offset, s.map_size, s.map_flags, &mapped_first);
    REQUIRE(first_map_result == VK_SUCCESS);
    REQUIRE(mapped_first != nullptr);
    ::vkfwd::forwarder::generated::vkUnmapMemory_entry(s.device, s.memory);
    REQUIRE(s.saw_unmap_request);

    // Re-install the transport so call_count resets to 0 and the next entry
    // is treated as a fresh map; this also clears the request stream which
    // unmap left empty after flush returned.
    s.call_count        = 0;
    s.saw_map_request   = false;
    s.saw_unmap_request = false;
    install_pack_unpack_transport(handle_map_then_unmap);

    // Second map: a fresh vm::reserve must succeed cleanly. We do not assert
    // pointer equality with the first map — VA reuse depends on the host OS.
    void *         mapped_second = nullptr;
    const VkResult second_map_result =
        ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.map_offset, s.map_size, s.map_flags, &mapped_second);
    CHECK(second_map_result == VK_SUCCESS);
    REQUIRE(mapped_second != nullptr);

    // Same alignment contract as the original map: *ppData - offset must be
    // page-aligned for any subsequent host writes to satisfy the Vulkan spec.
    const auto base_address = reinterpret_cast<std::uintptr_t>(mapped_second) - s.map_offset;
    CHECK(base_address % s.min_memory_map_alignment == 0);

    // Tear down the second mapping so the singleton manager does not retain a
    // live reservation across test boundaries.
    ::vkfwd::forwarder::generated::vkUnmapMemory_entry(s.device, s.memory);
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

} // namespace vkfwd::forwarder::test
