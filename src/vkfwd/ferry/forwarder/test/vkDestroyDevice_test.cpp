#include "support.hpp"

#include "generated/command/vkDestroyDevice.hpp"
#include "generated/forwarder_entrypoints.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using Command = ::vkfwd::generated::commands::vkDestroyDevice::Command;

struct Scenario {
    int                   allocator_user_data = 0x505;
    VkAllocationCallbacks allocator;
    VkDevice              device = test_handle<VkDevice>(0x505);
};

Scenario make_scenario() {
    Scenario scenario;
    scenario.allocator = test_allocator(&scenario.allocator_user_data);
    return scenario;
}

Scenario & scenario() {
    static Scenario value = make_scenario();
    return value;
}

CommandStream handle_flush(CommandStream & request_stream) {
    auto &     expected = scenario();
    const auto packet   = first_command_chunk(request_stream);

    const auto   parameters_offset = command_payload_blob_offset<Command::Parameters>(packet);
    const auto   allocator_offset  = field_relative_target_offset(request_stream, parameters_offset, &Command::Parameters::pAllocator);
    const auto & raw_parameters    = object_at<Command::Parameters>(request_stream, parameters_offset);
    check_field_relative_pointer(request_stream, parameters_offset, &Command::Parameters::pAllocator, allocator_offset);
    CHECK(raw_parameters.pAllocator == nullptr);

    auto                        command_bytes = command_view(request_stream, packet);
    const Command::Parameters * actual        = nullptr;
    REQUIRE(Command::unpack_parameters(command_bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->device == expected.device);
    CHECK(actual->pAllocator == nullptr);

    // Deferrable generated commands acknowledge successful transport processing
    // with an empty response stream; there is no response packet to unpack.
    return CommandStream {};
}

} // namespace

TEST_CASE("vkDestroyDevice forwarder entry point packs parameters when flushed") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_flush);

    vkfwd::forwarder::generated::vkDestroyDevice_entry(expected.device, &expected.allocator);
    CommandStream response_stream = Forwarder::instance().flush();

    CHECK(transport_state().processed);
    CHECK(response_stream.size() == 0);
}

} // namespace vkfwd::forwarder::test
