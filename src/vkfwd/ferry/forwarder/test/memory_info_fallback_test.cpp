#include "support.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_type_registry.hpp"
#include "memory_map/wire_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::forwarder::test {
namespace {

using AllocateCommand = ::vkfwd::generated::commands::vkAllocateMemory::Command;
using ::vkfwd::kMemoryMapManagerRevision;
using ::vkfwd::memory_map::wire::MemoryMapResponse;
using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoRequest;
using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoResponse;

constexpr VkDeviceSize kAllocationSize = 64 * 1024;

// Shared state for the static-storage flush handlers, since
// install_pack_unpack_transport requires a plain function pointer. The
// registry singleton persists across the entire test binary, so every test
// here uses a UNIQUE physical_device handle to avoid stepping on the
// cached rows other tests record (notably vkAllocateFreeMemory_test which
// pre-populates 0x401).
struct Scenario {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkDeviceMemory   receiver_memory = VK_NULL_HANDLE;

    VkMemoryAllocateInfo allocate_info {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = kAllocationSize,
        .memoryTypeIndex = 0,
    };

    // Query-response knobs.
    VkResult                         query_status                   = VK_SUCCESS;
    VkPhysicalDeviceMemoryProperties query_memory_properties        {};
    VkDeviceSize                     query_non_coherent_atom_size   = 64;
    std::uint64_t                    query_min_memory_map_alignment = 4096;

    // Observed wire traffic for assertions.
    int  call_count    = 0;
    bool saw_allocate  = false;
    bool saw_query     = false;
    bool saw_map       = false;
    bool saw_unexpected_chunk = false;

    QueryPhysicalDeviceMemoryInfoRequest seen_query_request {};
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

// Synthesize the receiver-side response for vkAllocateMemory: just packs the
// generated Response shape so the entry path unpacks a valid VkDeviceMemory.
CommandStream make_allocate_response(const Scenario & s) {
    CommandStream             response_stream;
    AllocateCommand::Response response {.return_value = VK_SUCCESS, .pMemory = &const_cast<Scenario &>(s).receiver_memory};
    REQUIRE(AllocateCommand::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

// Synthesize a QueryPhysicalDeviceMemoryInfoResponse the forwarder reads via
// at<>(0, ...). First grow on an empty stream lands at offset 0 because
// CommandStream::kBaseAlignment is a multiple of the response's alignof.
CommandStream make_query_response(const Scenario & s) {
    CommandStream                         stream;
    QueryPhysicalDeviceMemoryInfoResponse response {};
    response.manager_revision         = kMemoryMapManagerRevision;
    response.return_value             = static_cast<std::int32_t>(s.query_status);
    response.memory_properties        = s.query_memory_properties;
    response.non_coherent_atom_size   = s.query_non_coherent_atom_size;
    response.min_memory_map_alignment = s.query_min_memory_map_alignment;

    auto view = stream.grow<QueryPhysicalDeviceMemoryInfoResponse>(1, alignof(QueryPhysicalDeviceMemoryInfoResponse));
    view.set(0, response);
    return stream;
}

CommandStream make_map_response(const Scenario & /*s*/, VkDeviceSize effective_size) {
    CommandStream     stream;
    auto              view = stream.grow<MemoryMapResponse>(1, alignof(MemoryMapResponse));
    MemoryMapResponse response {
        .manager_revision = kMemoryMapManagerRevision,
        .return_value     = static_cast<std::int32_t>(VK_SUCCESS),
        .effective_size   = effective_size,
    };
    view.set(0, response);
    return stream;
}

// Pull the first chunk's command_id without further parsing — used by the
// stateful dispatcher to pick the right response branch.
std::uint32_t peek_command_id(CommandStream & request_stream) {
    const Range  packet = first_command_range(request_stream);
    auto         bytes  = command_view(request_stream, packet);
    REQUIRE(bytes.size() >= sizeof(CommandChunkHeader));
    CommandChunkHeader header {};
    std::memcpy(&header, bytes.address(0), sizeof(header));
    return header.command_id;
}

// Stateful dispatcher: vkAllocateMemory triggers one flush; the fallback
// triggers a second flush with a QueryPhysicalDeviceMemoryInfo chunk; map()
// triggers a third with a MemoryMap chunk. Branch on command_id so the
// per-test driver can issue map after allocate without installing a new
// handler.
CommandStream stateful_handler(CommandStream & request_stream) {
    auto &              s          = scenario();
    const std::uint32_t command_id = peek_command_id(request_stream);
    ++s.call_count;

    if (command_id == static_cast<std::uint32_t>(::vkfwd::generated::CommandId::AllocateMemory)) {
        s.saw_allocate = true;
        return make_allocate_response(s);
    }
    if (command_id == static_cast<std::uint32_t>(::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo)) {
        s.saw_query = true;
        // Capture the query payload so tests can confirm the physical_device
        // round-tripped correctly.
        const Range packet = first_command_range(request_stream);
        auto        bytes  = command_view(request_stream, packet);
        constexpr std::size_t kPayloadAlignment = alignof(QueryPhysicalDeviceMemoryInfoRequest);
        constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
        REQUIRE(bytes.size() >= kPayloadOffset + sizeof(QueryPhysicalDeviceMemoryInfoRequest));
        std::memcpy(&s.seen_query_request, bytes.address(kPayloadOffset), sizeof(s.seen_query_request));
        return make_query_response(s);
    }
    if (command_id == static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryMap)) {
        s.saw_map = true;
        // Echo the requested map size so the forwarder's strict size-match
        // check passes. We can pull it from the wire payload to avoid hard-
        // coding it in the test.
        const Range packet = first_command_range(request_stream);
        auto        bytes  = command_view(request_stream, packet);
        constexpr std::size_t kPayloadAlignment = alignof(::vkfwd::memory_map::wire::MemoryMapRequest);
        constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
        ::vkfwd::memory_map::wire::MemoryMapRequest map_req {};
        REQUIRE(bytes.size() >= kPayloadOffset + sizeof(map_req));
        std::memcpy(&map_req, bytes.address(kPayloadOffset), sizeof(map_req));
        return make_map_response(s, map_req.size);
    }
    s.saw_unexpected_chunk = true;
    FAIL("stateful_handler: unexpected command_id " << command_id);
    return {};
}

// Handler for the "no fallback should fire" test. Must not see a query chunk.
CommandStream allocate_only_handler(CommandStream & request_stream) {
    auto &              s          = scenario();
    const std::uint32_t command_id = peek_command_id(request_stream);
    ++s.call_count;
    if (command_id == static_cast<std::uint32_t>(::vkfwd::generated::CommandId::AllocateMemory)) {
        s.saw_allocate = true;
        return make_allocate_response(s);
    }
    if (command_id == static_cast<std::uint32_t>(::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo)) {
        s.saw_query = true;
        FAIL("allocate_only_handler: fallback emitted QueryPhysicalDeviceMemoryInfo but should not have");
        return CommandStream {};
    }
    s.saw_unexpected_chunk = true;
    FAIL("allocate_only_handler: unexpected command_id " << command_id);
    return {};
}

// Reset both the global registry rows we touch and the manager record so
// successive tests start from a known empty state. The registry singleton
// persists across tests in the same process.
void clean_state(const Scenario & s) {
    auto & manager  = ::vkfwd::MemoryMapForwarder::instance();
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    manager.forget_allocation(s.receiver_memory);
    registry.forget_device(s.device);
    // record_memory_properties / record_non_coherent_atom_size /
    // record_min_memory_map_alignment are keyed by physical_device. The
    // registry has no public "forget the physical-device row" surface, but the
    // resolve() path requires the device row to exist first — so as long as
    // each test calls forget_device(), resolve() misses regardless of
    // residual physical-device rows. The fallback then refills them.
}

VkPhysicalDeviceMemoryProperties make_host_visible_properties() {
    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 1;
    props.memoryTypes[0]  = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    return props;
}

} // namespace

TEST_CASE("memory_info_fallback: registry miss triggers query, retry resolves, allocation is tracked") {
    auto & s                  = scenario();
    s                         = Scenario {};
    // Unique per-test handles so neither the registry's physical-device rows
    // nor the manager's per-memory record collide with other tests in this
    // process. The registry singleton has no public "forget physical_device"
    // surface; uniqueness is the simplest workaround.
    s.physical_device         = test_handle<VkPhysicalDevice>(0xFA01);
    s.device                  = test_handle<VkDevice>(0xFA11);
    s.receiver_memory         = test_handle<VkDeviceMemory>(0xFA21);
    s.query_memory_properties = make_host_visible_properties();

    clean_state(s);
    // Only record the device->physical link; the physical->properties row is
    // missing so the first resolve() returns nullopt and triggers the
    // fallback. (We DO need device_to_physical_, otherwise the helper has
    // nowhere to send the query.)
    ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_device(s.device, s.physical_device);

    install_pack_unpack_transport(stateful_handler);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkAllocateMemory_entry(s.device, &s.allocate_info, nullptr, &memory);

    REQUIRE(result == VK_SUCCESS);
    CHECK(memory == s.receiver_memory);
    CHECK(s.saw_allocate);
    CHECK(s.saw_query);
    CHECK_FALSE(s.saw_unexpected_chunk);
    CHECK(s.seen_query_request.physical_device == s.physical_device);

    // The fallback should have populated the registry, and the retry should
    // have classified the allocation. Manager bookkeeping is the observable
    // proof: record_allocation only runs when resolve() returns a value.
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == s.allocate_info.allocationSize);

    // A follow-up vkMapMemory through the same transport should reach the
    // map_endpoint path (because the allocation is tracked), not the
    // VK_ERROR_FEATURE_NOT_PRESENT short-circuit in the manager surface.
    void *         mapped     = nullptr;
    const VkResult map_result = vkfwd::forwarder::generated::vkMapMemory_entry(memory ? s.device : VK_NULL_HANDLE, memory, 0, 4096, 0, &mapped);
    CHECK(map_result == VK_SUCCESS);
    CHECK(s.saw_map);
    REQUIRE(mapped != nullptr);

    // Clean up state for the next test.
    clean_state(s);
}

TEST_CASE("memory_info_fallback: receiver returns non-success, allocation stays untracked") {
    auto & s                  = scenario();
    s                         = Scenario {};
    // Distinct from the success-path handles above so neither registry rows
    // nor manager records collide.
    s.physical_device         = test_handle<VkPhysicalDevice>(0xFB01);
    s.device                  = test_handle<VkDevice>(0xFB11);
    s.receiver_memory         = test_handle<VkDeviceMemory>(0xFB21);
    s.query_status            = VK_ERROR_INITIALIZATION_FAILED;
    s.query_memory_properties = make_host_visible_properties();

    clean_state(s);
    ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_device(s.device, s.physical_device);

    install_pack_unpack_transport(stateful_handler);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkAllocateMemory_entry(s.device, &s.allocate_info, nullptr, &memory);

    // vkfwd must NOT propagate a classification failure as an allocation
    // failure: the receiver's allocate succeeded, the application gets its
    // VkDeviceMemory, only manager bookkeeping is skipped.
    REQUIRE(result == VK_SUCCESS);
    CHECK(memory == s.receiver_memory);
    CHECK(s.saw_allocate);
    CHECK(s.saw_query);
    CHECK_FALSE(s.saw_map);
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == 0);

    // The untracked allocation must surface VK_ERROR_FEATURE_NOT_PRESENT for
    // map() — that's the contract the hook docs everyone leans on.
    void *         mapped     = reinterpret_cast<void *>(0xdead);
    const VkResult map_result = vkfwd::forwarder::generated::vkMapMemory_entry(s.device, memory, 0, 4096, 0, &mapped);
    CHECK(map_result == VK_ERROR_FEATURE_NOT_PRESENT);

    clean_state(s);
}

TEST_CASE("memory_info_fallback: no physical_device known, fallback is a no-op") {
    auto & s          = scenario();
    s                 = Scenario {};
    s.physical_device = test_handle<VkPhysicalDevice>(0xFC01);
    s.device          = test_handle<VkDevice>(0xFC11);
    s.receiver_memory = test_handle<VkDeviceMemory>(0xFC21);

    clean_state(s);
    // Intentionally do NOT record_device, so physical_device_for() returns
    // nullopt and the helper short-circuits before sending any chunk.
    install_pack_unpack_transport(allocate_only_handler);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkAllocateMemory_entry(s.device, &s.allocate_info, nullptr, &memory);

    REQUIRE(result == VK_SUCCESS);
    CHECK(memory == s.receiver_memory);
    CHECK(s.saw_allocate);
    CHECK_FALSE(s.saw_query);
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == 0);

    clean_state(s);
}

} // namespace vkfwd::forwarder::test
