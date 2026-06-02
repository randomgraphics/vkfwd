#include "support.hpp"

#include "generated/command/vkDestroyInstance.hpp"
#include "generated/forwarder_entrypoints.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using Command = ::vkfwd::generated::commands::vkDestroyInstance::Command;

struct Scenario {
    int                   allocator_user_data = 0x404;
    VkAllocationCallbacks allocator;
    VkInstance            instance = test_handle<VkInstance>(0x404);
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

Blob handle_flush(Blob & request_blob) {
    auto &     expected = scenario();
    const auto packet   = first_command_chunk(request_blob);

    const auto parameters_offset = command_payload_blob_offset<Command::Parameters>(packet);
    const auto allocator_offset  = field_relative_target_offset(request_blob, parameters_offset, &Command::Parameters::pAllocator);
    const auto & raw_parameters  = object_at<Command::Parameters>(request_blob, parameters_offset);
    check_field_relative_pointer(request_blob, parameters_offset, &Command::Parameters::pAllocator, allocator_offset);
    CHECK(raw_parameters.pAllocator == nullptr);

    auto                        command_bytes = command_view(request_blob, packet);
    const Command::Parameters * actual        = nullptr;
    REQUIRE(Command::unpack_parameters(command_bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->instance == expected.instance);
    CHECK(actual->pAllocator == nullptr);

    // Deferrable generated commands acknowledge successful transport processing
    // with an empty response blob; there is no response packet to unpack.
    return Blob {};
}

} // namespace

TEST_CASE("vkDestroyInstance forwarder entry point packs parameters when flushed") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_flush);

    vkfwd::forwarder::generated::vkDestroyInstance_entry(expected.instance, &expected.allocator);
    Blob response_blob = Forwarder::instance().flush();

    CHECK(transport_state().processed);
    CHECK(response_blob.size() == 0);
}

} // namespace vkfwd::forwarder::test
