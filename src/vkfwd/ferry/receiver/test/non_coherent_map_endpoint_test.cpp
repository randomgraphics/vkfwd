#include "command_stream.hpp"
#include "custom_command.hpp"
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
using ::vkfwd::memory_map::wire::MemoryMapRequest;
using ::vkfwd::memory_map::wire::MemoryMapResponse;

template<class Handle>
Handle test_handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

// Recording state for the map_memory PFN. The dispatch table holds a plain
// function pointer with no closure, so the stub forwards through this static
// scenario object that each TEST_CASE resets at the top.
struct MapMemoryStub {
    bool             called       = false;
    VkDevice         saw_device   = VK_NULL_HANDLE;
    VkDeviceMemory   saw_memory   = VK_NULL_HANDLE;
    VkDeviceSize     saw_offset   = 0;
    VkDeviceSize     saw_size     = 0;
    VkMemoryMapFlags saw_flags    = 0;
    VkResult         return_value = VK_SUCCESS;
    // The driver writes a non-null receiver-process address into *ppData on
    // success. The endpoint must store this internally and never echo it back
    // through the wire — only effective_size and the VkResult travel.
    void * fake_mapped_ptr = nullptr;
};

MapMemoryStub & stub() {
    static MapMemoryStub state;
    return state;
}

VKAPI_ATTR VkResult VKAPI_CALL stub_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                                void ** ppData) {
    auto & s     = stub();
    s.called     = true;
    s.saw_device = device;
    s.saw_memory = memory;
    s.saw_offset = offset;
    s.saw_size   = size;
    s.saw_flags  = flags;
    if (ppData) { *ppData = s.fake_mapped_ptr; }
    return s.return_value;
}

// Build a single-chunk request CommandStream containing a MemoryMap manual
// command. Returns the Range covering the chunk so the endpoint can find it.
// Mirrors the forwarder-side append_manual_chunk template; we keep a local copy
// so the test does not link against forwarder internals.
Range append_memory_map_chunk(CommandStream & stream, const MemoryMapRequest & request, std::uint32_t command_revision = kMemoryMapManagerRevision) {
    constexpr std::size_t kPayloadAlignment = alignof(MemoryMapRequest);
    constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kChunkSize        = kPayloadOffset + sizeof(MemoryMapRequest);

    std::size_t offset      = 0;
    auto        destination = stream.grow<std::uint8_t>(kChunkSize, CommandStream::kBaseAlignment, &offset);

    CommandChunkHeader header {};
    header.command_id       = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryMap);
    header.size             = static_cast<std::uint32_t>(kChunkSize);
    header.command_revision = command_revision;
    REQUIRE(destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t *>(&header)) == sizeof(header));
    REQUIRE(destination.set(kPayloadOffset, sizeof(request), reinterpret_cast<const std::uint8_t *>(&request)) == sizeof(request));
    return Range {.offset = offset, .size = static_cast<std::uint32_t>(kChunkSize)};
}

// Default classification matches a non-coherent host-visible allocation so the
// receiver factory routes through NonCoherentReceiverAllocation. Tests that
// want different behavior override individual fields after copying this base.
MemoryMapRequest make_request(VkDevice device, VkDeviceMemory memory) {
    return MemoryMapRequest {
        .manager_revision         = kMemoryMapManagerRevision,
        .memory_type_index        = 2,
        .device                   = device,
        .memory                   = memory,
        .offset                   = 0,
        .size                     = 4096,
        .flags                    = 0,
        .pad0                     = 0,
        .property_flags           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        .pad1                     = 0,
        .allocation_size          = 64 * 1024,
        .non_coherent_atom_size   = 64,
        .min_memory_map_alignment = 4096,
    };
}

MemoryMapResponse read_response(const CommandStream & response_stream) {
    REQUIRE(response_stream.size() >= sizeof(MemoryMapResponse));
    auto view = response_stream.at<MemoryMapResponse>(0, sizeof(MemoryMapResponse));
    REQUIRE(!view.empty());
    MemoryMapResponse response {};
    std::memcpy(&response, view.address(0), sizeof(response));
    return response;
}

void install_stub(ReplayContext & replay_context) {
    // Direct PFN assignment is the lightest-weight option here: the dispatch
    // table exposes map_memory as a public PFN slot, and init() would only
    // accept a real get_device_proc_addr (a heavier path with more moving
    // parts than the test needs).
    replay_context.dispatch.device.map_memory = stub_vkMapMemory;
}

} // namespace

TEST_CASE("NonCoherentReceiverAllocation::map_endpoint dispatches to driver and packs success response") {
    auto & s          = stub();
    s                 = MapMemoryStub {};
    s.return_value    = VK_SUCCESS;
    s.fake_mapped_ptr = reinterpret_cast<void *>(0xC0FE0000);

    const VkDevice       source_device   = test_handle<VkDevice>(0xD0D0);
    const VkDevice       receiver_device = test_handle<VkDevice>(0xE0E0);
    const VkDeviceMemory source_memory   = test_handle<VkDeviceMemory>(0xA0A0);
    const VkDeviceMemory receiver_memory = test_handle<VkDeviceMemory>(0xB0B0);

    ReplayContext context;
    context.source_to_receiver_device[source_device] = receiver_device;
    context.source_to_receiver_memory[source_memory] = receiver_memory;
    install_stub(context);

    MemoryMapRequest req = make_request(source_device, source_memory);
    req.offset           = 1024;
    req.size             = 8192;
    req.flags            = 0;

    CommandStream request_stream;
    const Range   chunk = append_memory_map_chunk(request_stream, req);

    CommandStream     response_stream;
    MemoryMapReceiver receiver;
    const bool        ok = receiver.custom_vkMapMemory_endpoint(request_stream, chunk, response_stream, context);
    REQUIRE(ok);

    // The endpoint translates handles before calling the driver. Verify the
    // PFN received the receiver-native values rather than the source-visible
    // ones the source process supplied.
    CHECK(s.called);
    CHECK(s.saw_device == receiver_device);
    CHECK(s.saw_memory == receiver_memory);
    CHECK(s.saw_offset == req.offset);
    CHECK(s.saw_size == req.size);
    CHECK(s.saw_flags == req.flags);

    // Wire response carries success and the receiver-resolved effective_size.
    // The fake mapped pointer must NOT appear anywhere in the response — it is
    // a receiver-process address and would be a security/correctness leak.
    const auto response = read_response(response_stream);
    CHECK(response.manager_revision == kMemoryMapManagerRevision);
    CHECK(response.return_value == VK_SUCCESS);
    CHECK(response.effective_size == req.size);
}

TEST_CASE("NonCoherentReceiverAllocation::map_endpoint surfaces driver failure verbatim") {
    auto & s          = stub();
    s                 = MapMemoryStub {};
    s.return_value    = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    s.fake_mapped_ptr = nullptr;

    const VkDevice       source_device   = test_handle<VkDevice>(0xD1D1);
    const VkDevice       receiver_device = test_handle<VkDevice>(0xE1E1);
    const VkDeviceMemory source_memory   = test_handle<VkDeviceMemory>(0xA1A1);
    const VkDeviceMemory receiver_memory = test_handle<VkDeviceMemory>(0xB1B1);

    ReplayContext context;
    context.source_to_receiver_device[source_device] = receiver_device;
    context.source_to_receiver_memory[source_memory] = receiver_memory;
    install_stub(context);

    MemoryMapRequest req = make_request(source_device, source_memory);

    CommandStream request_stream;
    const Range   chunk = append_memory_map_chunk(request_stream, req);
    CommandStream response_stream;

    MemoryMapReceiver receiver;
    const bool        ok = receiver.custom_vkMapMemory_endpoint(request_stream, chunk, response_stream, context);
    // Returns true to keep the session alive — the driver-rejected map is a
    // per-call failure, not a protocol error.
    REQUIRE(ok);
    CHECK(s.called);

    const auto response = read_response(response_stream);
    CHECK(response.return_value == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    // effective_size must be zero on failure so the forwarder's strict equality
    // check between requested and reported sizes does not accidentally pass.
    CHECK(response.effective_size == 0);
}

TEST_CASE("MemoryMapReceiver::custom_vkMapMemory_endpoint rejects manager_revision mismatch without calling the driver") {
    auto & s       = stub();
    s              = MapMemoryStub {};
    s.return_value = VK_SUCCESS;

    const VkDevice       source_device   = test_handle<VkDevice>(0xD2D2);
    const VkDevice       receiver_device = test_handle<VkDevice>(0xE2E2);
    const VkDeviceMemory source_memory   = test_handle<VkDeviceMemory>(0xA2A2);
    const VkDeviceMemory receiver_memory = test_handle<VkDeviceMemory>(0xB2B2);

    ReplayContext context;
    context.source_to_receiver_device[source_device] = receiver_device;
    context.source_to_receiver_memory[source_memory] = receiver_memory;
    install_stub(context);

    MemoryMapRequest req = make_request(source_device, source_memory);
    req.manager_revision = 999; // any value != kMemoryMapManagerRevision

    CommandStream request_stream;
    const Range   chunk = append_memory_map_chunk(request_stream, req);
    CommandStream response_stream;

    MemoryMapReceiver receiver;
    const bool        ok = receiver.custom_vkMapMemory_endpoint(request_stream, chunk, response_stream, context);
    REQUIRE(ok);

    // The endpoint must short-circuit on revision mismatch BEFORE touching the
    // local Vulkan driver: a request from an incompatible source could carry
    // misclassified flags, and dispatching anyway risks subtle behavior drift.
    CHECK_FALSE(s.called);

    const auto response = read_response(response_stream);
    CHECK(response.manager_revision == kMemoryMapManagerRevision);
    CHECK(response.return_value == VK_ERROR_UNKNOWN);
    CHECK(response.effective_size == 0);
}

} // namespace vkfwd::receiver::test
