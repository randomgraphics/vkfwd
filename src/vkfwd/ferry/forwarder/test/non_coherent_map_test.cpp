#include "support.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/vm_primitives.hpp"
#include "memory_map/wire_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::forwarder::test {
namespace {

using ::vkfwd::kMemoryMapManagerRevision;
using ::vkfwd::memory_map::wire::MemoryMapRequest;
using ::vkfwd::memory_map::wire::MemoryMapResponse;

// All map() tests share one set of allocation parameters so the static-storage
// flush handlers (install_pack_unpack_transport requires plain function
// pointers) can read the expected values without captures.
struct Scenario {
    VkDevice              device                   = test_handle<VkDevice>(0x9001);
    VkDeviceMemory        memory                   = test_handle<VkDeviceMemory>(0xA001);
    VkMemoryPropertyFlags property_flags           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    std::uint32_t         memory_type_index        = 2;
    VkDeviceSize          allocation_size          = 64 * 1024;
    VkDeviceSize          non_coherent_atom_size   = 64;
    std::size_t           min_memory_map_alignment = 4096;
    VkDeviceSize          offset                   = 0;
    VkDeviceSize          size                     = VK_WHOLE_SIZE;
    VkMemoryMapFlags      flags                    = 0;

    // Response synthesis knobs the per-test setup writes before driving the entry.
    VkResult         response_status         = VK_SUCCESS;
    VkDeviceSize     response_effective_size = 0;
    std::uint32_t    response_revision       = kMemoryMapManagerRevision;
    bool             response_omit_payload   = false;
    bool             saw_request             = false;
    std::uint32_t    saw_command_id          = 0;
    std::uint32_t    saw_revision            = 0;
    MemoryMapRequest saw_request_payload {};
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

// Synthesizes a receiver-side response: a CommandStream whose first bytes are
// a MemoryMapResponse the forwarder reads via at<>() at offset 0.
CommandStream make_response_stream(const Scenario & s) {
    CommandStream stream;
    if (s.response_omit_payload) { return stream; }
    auto              view = stream.grow<MemoryMapResponse>(1, alignof(MemoryMapResponse));
    MemoryMapResponse response {
        .manager_revision = s.response_revision,
        .return_value     = static_cast<std::int32_t>(s.response_status),
        .effective_size   = s.response_effective_size,
    };
    view.set(0, response);
    return stream;
}

CommandStream handle_map_flush(CommandStream & request_stream) {
    auto &      s      = scenario();
    const Range packet = first_command_range(request_stream);
    auto        bytes  = command_view(request_stream, packet);

    // CommandChunkHeader sits at the start of the chunk; the MemoryMapRequest
    // payload follows at alignof(MemoryMapRequest) (=8) which equals
    // sizeof(CommandChunkHeader)=16 — both are multiples of 8.
    REQUIRE(bytes.size() >= sizeof(CommandChunkHeader) + sizeof(MemoryMapRequest));
    CommandChunkHeader header {};
    std::memcpy(&header, bytes.address(0), sizeof(header));
    s.saw_command_id = header.command_id;
    s.saw_revision   = header.command_revision;

    constexpr std::size_t kPayloadOffset = (sizeof(CommandChunkHeader) + alignof(MemoryMapRequest) - 1) & ~(alignof(MemoryMapRequest) - 1);
    std::memcpy(&s.saw_request_payload, bytes.address(kPayloadOffset), sizeof(MemoryMapRequest));
    s.saw_request = true;

    return make_response_stream(s);
}

CommandStream handle_must_not_flush(CommandStream & /*request*/) {
    FAIL("transport must not be invoked when the local reserve/commit step fails");
    return {};
}

} // namespace

TEST_CASE("NonCoherentForwarderAllocation::map success: reserve+commit, send chunk, write *ppData") {
    auto & s                  = scenario();
    s                         = Scenario {};
    s.offset                  = 256;
    s.size                    = 4096;
    s.response_effective_size = 4096;

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_flush);

    void *         mapped = nullptr;
    const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

    CHECK(result == VK_SUCCESS);
    REQUIRE(mapped != nullptr);

    // Source-side staging is a VA reservation rooted at a page-aligned base.
    // The Vulkan contract requires `*ppData - offset` to be a multiple of
    // minMemoryMapAlignment. Page alignment (>= 4096) trivially satisfies any
    // common Vulkan minMemoryMapAlignment.
    const auto base_address = reinterpret_cast<std::uintptr_t>(mapped) - s.offset;
    CHECK(base_address % s.min_memory_map_alignment == 0);

    // Confirm the manual chunk landed on the wire with the right id, revision,
    // and classification fields.
    REQUIRE(s.saw_request);
    CHECK(s.saw_command_id == static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryMap));
    CHECK(s.saw_revision == kMemoryMapManagerRevision);
    CHECK(s.saw_request_payload.device == s.device);
    CHECK(s.saw_request_payload.memory == s.memory);
    CHECK(s.saw_request_payload.offset == s.offset);
    // The forwarder resolves VK_WHOLE_SIZE locally before sending; the wire
    // payload always carries an explicit byte count. For this non-WHOLE_SIZE
    // case the wire value is just the requested size.
    CHECK(s.saw_request_payload.size == s.size);
    CHECK(s.saw_request_payload.property_flags == s.property_flags);
    CHECK(s.saw_request_payload.memory_type_index == s.memory_type_index);
    CHECK(s.saw_request_payload.allocation_size == s.allocation_size);
    CHECK(s.saw_request_payload.non_coherent_atom_size == s.non_coherent_atom_size);
    CHECK(s.saw_request_payload.min_memory_map_alignment == s.min_memory_map_alignment);

    // Cleanup: forget the allocation so subsequent tests start clean. unmap()
    // is a Phase-1 Task-8 no-op so we explicitly forget; without forget the
    // shared singleton would retain a leaked reservation across test cases.
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

TEST_CASE("NonCoherentForwarderAllocation::map alignment: *ppData - offset is page-aligned for any offset") {
    auto & s = scenario();

    // Sweep offsets that probe the page-floor/page-ceil branches: zero,
    // sub-page, page-boundary, just past it, and a multi-page offset.
    const VkDeviceSize offsets[] = {0, 1, 256, 4097, 8192};

    for (const VkDeviceSize off : offsets) {
        s                         = Scenario {};
        s.offset                  = off;
        s.size                    = 1024;
        s.response_effective_size = 1024;

        record_scenario_allocation(s);
        install_pack_unpack_transport(handle_map_flush);

        void *         mapped = nullptr;
        const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);
        REQUIRE(result == VK_SUCCESS);
        REQUIRE(mapped != nullptr);

        const auto base_address = reinterpret_cast<std::uintptr_t>(mapped) - s.offset;
        CHECK(base_address % s.min_memory_map_alignment == 0);

        ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
    }
}

TEST_CASE("NonCoherentForwarderAllocation::map: vm::reserve and vm::commit failures release+return OOM, no wire activity") {
    namespace vm = ::vkfwd::memory_map::vm;

    SECTION("vm::reserve failure") {
        auto & s                  = scenario();
        s                         = Scenario {};
        s.offset                  = 0;
        s.size                    = 4096;
        s.response_effective_size = 4096;

        record_scenario_allocation(s);
        install_pack_unpack_transport(handle_must_not_flush);

        // The hook is consumed on the next reserve() call and reset to null;
        // verifying it returns to null after the entry confirms the failure
        // path actually went through vm::reserve.
        vm::g_test_reserve_failure_hook = [](std::size_t) -> void * { return nullptr; };

        void *         mapped = reinterpret_cast<void *>(0xdead);
        const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

        CHECK(result == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(mapped == nullptr);
        CHECK(vm::g_test_reserve_failure_hook == nullptr); // hook consumed
        CHECK_FALSE(transport_state().processed);

        ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
    }

    SECTION("vm::commit failure") {
        auto & s                  = scenario();
        s                         = Scenario {};
        s.offset                  = 0;
        s.size                    = 4096;
        s.response_effective_size = 4096;

        record_scenario_allocation(s);
        install_pack_unpack_transport(handle_must_not_flush);

        // The hook receives the page-aligned base+size the impl computed; we
        // do not perform the real commit, just signal failure. The reservation
        // is still in place so map() must call vm::release to clean up.
        vm::g_test_commit_failure_hook = [](void *, std::size_t) -> bool { return false; };

        void *         mapped = reinterpret_cast<void *>(0xdead);
        const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

        CHECK(result == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(mapped == nullptr);
        CHECK(vm::g_test_commit_failure_hook == nullptr);
        CHECK_FALSE(transport_state().processed);

        ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
    }
}

TEST_CASE("NonCoherentForwarderAllocation::map: receiver rejection releases the reservation and propagates the error") {
    auto & s                  = scenario();
    s                         = Scenario {};
    s.offset                  = 0;
    s.size                    = 4096;
    s.response_status         = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    s.response_effective_size = 0; // value irrelevant when status != VK_SUCCESS

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_flush);

    void *         mapped = reinterpret_cast<void *>(0xdead);
    const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

    CHECK(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(mapped == nullptr);
    REQUIRE(s.saw_request);

    // A follow-up map that succeeds must observe a fresh reservation. The
    // rejected map cannot leave behind state that taints the next attempt; we
    // confirm by mapping again and checking the success path still gives a
    // page-aligned pointer (and that the success handler runs, i.e., a fresh
    // chunk reaches the wire).
    s.response_status         = VK_SUCCESS;
    s.response_effective_size = 4096;
    s.saw_request             = false;
    install_pack_unpack_transport(handle_map_flush);
    void *         mapped_retry = nullptr;
    const VkResult retry_result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped_retry);
    CHECK(retry_result == VK_SUCCESS);
    REQUIRE(mapped_retry != nullptr);
    CHECK(s.saw_request);
    const auto retry_base = reinterpret_cast<std::uintptr_t>(mapped_retry) - s.offset;
    CHECK(retry_base % s.min_memory_map_alignment == 0);

    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

TEST_CASE("NonCoherentForwarderAllocation::map: effective_size mismatch returns VK_ERROR_UNKNOWN and releases") {
    auto & s                  = scenario();
    s                         = Scenario {};
    s.offset                  = 0;
    s.size                    = 4096;
    s.response_status         = VK_SUCCESS;
    s.response_effective_size = 4096 + 8; // intentional divergence from the source value

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_flush);

    void *         mapped = reinterpret_cast<void *>(0xdead);
    const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

    CHECK(result == VK_ERROR_UNKNOWN);
    CHECK(mapped == nullptr);

    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

TEST_CASE("NonCoherentForwarderAllocation::map: response manager_revision mismatch returns VK_ERROR_UNKNOWN") {
    auto & s                  = scenario();
    s                         = Scenario {};
    s.offset                  = 0;
    s.size                    = 4096;
    s.response_status         = VK_SUCCESS;
    s.response_effective_size = 4096;
    s.response_revision       = kMemoryMapManagerRevision + 42; // any value != current

    record_scenario_allocation(s);
    install_pack_unpack_transport(handle_map_flush);

    void *         mapped = reinterpret_cast<void *>(0xdead);
    const VkResult result = ::vkfwd::forwarder::generated::vkMapMemory_entry(s.device, s.memory, s.offset, s.size, s.flags, &mapped);

    CHECK(result == VK_ERROR_UNKNOWN);
    CHECK(mapped == nullptr);

    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(s.memory);
}

} // namespace vkfwd::forwarder::test
