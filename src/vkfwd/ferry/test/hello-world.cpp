#include "loopback_session.hpp"

#include "forwarder.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace vkfwd::test {
namespace {

constexpr VkInstanceCreateFlags kExpectedFlags = 0xbad;
const auto                      kReceiverInstance = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xbeef));

class LoopbackTransportCreator {
public:
    explicit LoopbackTransportCreator(std::shared_ptr<TransportSession> transport): transport_(std::move(transport)) {}

    std::shared_ptr<TransportSession> operator()() const {
        // Forwarder keeps a process-wide transport cache, so this creator pins
        // the exact loopback session that is paired with the receiver below.
        return transport_;
    }

private:
    std::shared_ptr<TransportSession> transport_;
};

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateInstance(const VkInstanceCreateInfo * pCreateInfo, const VkAllocationCallbacks *, VkInstance * pInstance) {
    REQUIRE(pCreateInfo != nullptr);
    CHECK(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    CHECK(pCreateInfo->flags == kExpectedFlags);
    REQUIRE(pInstance != nullptr);
    *pInstance = kReceiverInstance;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkCreateInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateInstance); }
    return nullptr;
}

} // namespace

TEST_CASE("loopback hello world forwards vkCreateInstance through receiver replay", "[loopback]") {
    receiver::ReplayContext replay_context;
    replay_context.dispatch.global.init(fake_vkGetInstanceProcAddr);

    auto loopback = LoopbackSession::create();
    Receiver receiver(*loopback.receiver, replay_context);

    Forwarder::set_transport_creator(LoopbackTransportCreator(loopback.transport));

    VkInstanceCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = kExpectedFlags,
    };
    VkInstance instance = VK_NULL_HANDLE;

    const VkResult result = forwarder::generated::vkCreateInstance_entry(&create_info, nullptr, &instance);

    CHECK(result == VK_SUCCESS);
    CHECK(instance == kReceiverInstance);
}

} // namespace vkfwd::test
