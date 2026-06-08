#include "command_stream.hpp"
#include "custom_command.hpp"
#include "manual_dispatch.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/wire_format.hpp"
#include "replay_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vkfwd::receiver::test {
namespace {

using ::vkfwd::CommandChunkHeader;
using ::vkfwd::CommandStream;
using ::vkfwd::kMemoryMapManagerRevision;
using ::vkfwd::Range;
using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoRequest;
using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoResponse;

template<class Handle>
Handle test_handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

// Recording state for the two PFNs the dispatch invokes. The dispatch table
// holds a plain function pointer with no closure, so the stubs forward through
// a static scenario object each TEST_CASE resets up-front.
struct QueryStub {
    bool             memory_props_called = false;
    bool             phys_props_called   = false;
    VkPhysicalDevice memory_props_saw    = VK_NULL_HANDLE;
    VkPhysicalDevice phys_props_saw      = VK_NULL_HANDLE;
    // Driver-canned answers.
    VkPhysicalDeviceMemoryProperties memory_properties {};
    VkPhysicalDeviceProperties       device_properties {};
};

QueryStub & stub() {
    static QueryStub s;
    return s;
}

VKAPI_ATTR void VKAPI_CALL stub_get_memory_properties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties * pMemoryProperties) {
    auto & s              = stub();
    s.memory_props_called = true;
    s.memory_props_saw    = physicalDevice;
    if (pMemoryProperties) { *pMemoryProperties = s.memory_properties; }
}

VKAPI_ATTR void VKAPI_CALL stub_get_physical_device_properties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties * pProperties) {
    auto & s            = stub();
    s.phys_props_called = true;
    s.phys_props_saw    = physicalDevice;
    if (pProperties) { *pProperties = s.device_properties; }
}

// Build a single-chunk request CommandStream. Local copy of the forwarder-side
// helper so the test does not link against forwarder internals — same
// alignment rules apply because the receiver dispatch parses the same layout.
Range append_query_chunk(CommandStream & stream, const QueryPhysicalDeviceMemoryInfoRequest & request,
                         std::uint32_t command_revision = kMemoryMapManagerRevision) {
    constexpr std::size_t kPayloadAlignment = alignof(QueryPhysicalDeviceMemoryInfoRequest);
    constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kChunkSize        = kPayloadOffset + sizeof(QueryPhysicalDeviceMemoryInfoRequest);

    std::size_t offset      = 0;
    auto        destination = stream.grow<std::uint8_t>(kChunkSize, CommandStream::kBaseAlignment, &offset);

    CommandChunkHeader header {};
    header.command_id       = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo);
    header.size             = static_cast<std::uint32_t>(kChunkSize);
    header.command_revision = command_revision;
    REQUIRE(destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t *>(&header)) == sizeof(header));
    REQUIRE(destination.set(kPayloadOffset, sizeof(request), reinterpret_cast<const std::uint8_t *>(&request)) == sizeof(request));
    return Range {.offset = offset, .size = static_cast<std::uint32_t>(kChunkSize)};
}

QueryPhysicalDeviceMemoryInfoResponse read_response(const CommandStream & response_stream) {
    REQUIRE(response_stream.size() >= sizeof(QueryPhysicalDeviceMemoryInfoResponse));
    auto view = response_stream.at<QueryPhysicalDeviceMemoryInfoResponse>(0, sizeof(QueryPhysicalDeviceMemoryInfoResponse));
    REQUIRE(!view.empty());
    QueryPhysicalDeviceMemoryInfoResponse response {};
    std::memcpy(&response, view.address(0), sizeof(response));
    return response;
}

void install_stubs(ReplayContext & replay_context) {
    // Direct PFN assignment to keep the test focused on the dispatch body;
    // init() would require a real instance.
    replay_context.dispatch.instance.get_physical_device_memory_properties = stub_get_memory_properties;
    replay_context.dispatch.instance.get_physical_device_properties        = stub_get_physical_device_properties;
}

} // namespace

TEST_CASE("dispatch_manual_command(QueryPhysicalDeviceMemoryInfo) drives both PFNs and packs the response") {
    auto & s = stub();
    s        = QueryStub {};
    // Canned driver answers: two host-visible memory types and specific limits
    // values the assertion logic compares to the wire response.
    s.memory_properties.memoryTypeCount = 2;
    s.memory_properties.memoryTypes[0]  = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    s.memory_properties.memoryTypes[1]  = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1};
    s.memory_properties.memoryHeapCount = 1;
    s.memory_properties.memoryHeaps[0]  = {0x40000000, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    s.device_properties.limits.nonCoherentAtomSize   = 128;
    s.device_properties.limits.minMemoryMapAlignment = 4096;

    const VkPhysicalDevice source_pd   = test_handle<VkPhysicalDevice>(0xF1F1);
    const VkPhysicalDevice receiver_pd = test_handle<VkPhysicalDevice>(0xE2E2);

    ReplayContext context;
    context.source_to_receiver_physical_device[source_pd] = receiver_pd;
    install_stubs(context);

    QueryPhysicalDeviceMemoryInfoRequest req {
        .manager_revision = kMemoryMapManagerRevision,
        .physical_device  = source_pd,
    };
    CommandStream request_stream;
    const Range   chunk = append_query_chunk(request_stream, req);
    CommandStream response_stream;

    const bool ok =
        ::vkfwd::receiver::dispatch_manual_command(::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo, request_stream, chunk, response_stream, context);
    REQUIRE(ok);

    // Both PFNs called with the receiver-native handle (translation happened).
    CHECK(s.memory_props_called);
    CHECK(s.phys_props_called);
    CHECK(s.memory_props_saw == receiver_pd);
    CHECK(s.phys_props_saw == receiver_pd);

    // Response carries the driver's values verbatim.
    const auto response = read_response(response_stream);
    CHECK(response.manager_revision == kMemoryMapManagerRevision);
    CHECK(response.return_value == VK_SUCCESS);
    CHECK(response.memory_properties.memoryTypeCount == s.memory_properties.memoryTypeCount);
    CHECK(response.memory_properties.memoryTypes[0].propertyFlags == s.memory_properties.memoryTypes[0].propertyFlags);
    CHECK(response.memory_properties.memoryTypes[1].propertyFlags == s.memory_properties.memoryTypes[1].propertyFlags);
    CHECK(response.non_coherent_atom_size == s.device_properties.limits.nonCoherentAtomSize);
    CHECK(response.min_memory_map_alignment == s.device_properties.limits.minMemoryMapAlignment);
}

TEST_CASE("dispatch_manual_command(QueryPhysicalDeviceMemoryInfo) without handle mapping packs VK_ERROR_UNKNOWN") {
    auto & s = stub();
    s        = QueryStub {};

    ReplayContext context;
    // Intentionally do NOT populate source_to_receiver_physical_device — this
    // exercises the missing-handle protocol error branch that the
    // QueryPhysicalDeviceMemoryInfo forwarder helper logs but still treats as
    // a no-op (registry stays empty).
    install_stubs(context);

    QueryPhysicalDeviceMemoryInfoRequest req {
        .manager_revision = kMemoryMapManagerRevision,
        .physical_device  = test_handle<VkPhysicalDevice>(0xDEAD),
    };
    CommandStream request_stream;
    const Range   chunk = append_query_chunk(request_stream, req);
    CommandStream response_stream;

    const bool ok =
        ::vkfwd::receiver::dispatch_manual_command(::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo, request_stream, chunk, response_stream, context);
    REQUIRE(ok);

    // No driver call should have happened: handle translation failed first.
    CHECK_FALSE(s.memory_props_called);
    CHECK_FALSE(s.phys_props_called);

    const auto response = read_response(response_stream);
    CHECK(response.return_value == VK_ERROR_UNKNOWN);
}

} // namespace vkfwd::receiver::test
