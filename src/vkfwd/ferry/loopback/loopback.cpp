#include "loopback.hpp"

#include "generated/vulkan_api.hpp"

namespace vkfwd::loopback {
namespace {

class LoopbackTransportSession final: public TransportSession {
public:
    LoopbackTransportSession() {
        info_.local_handshake.vulkan_api_version  = generated::kVulkanApiVersion;
        info_.local_handshake.schema_version      = kSupportedSchemaVersion;
        info_.remote_handshake.vulkan_api_version = generated::kVulkanApiVersion;
        info_.remote_handshake.schema_version     = kSupportedSchemaVersion;
    }

    const TransportSessionInfo & info() const override { return info_; }

    Blob send_accumulated_api_calls(Blob &) override {
        return {};
    }

private:
    TransportSessionInfo info_;
};

} // namespace

std::shared_ptr<TransportSession> make_loopback_transport_session() { return std::make_shared<LoopbackTransportSession>(); }

} // namespace vkfwd::loopback
