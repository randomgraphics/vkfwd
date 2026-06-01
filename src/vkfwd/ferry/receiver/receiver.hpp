#pragma once

#include "receiver_session.hpp"

namespace vkfwd {

class Receiver {
public:
    Receiver(ReceiverSession & s);

    // Receiver replay is injectable so tests can validate dispatch ordering
    // without requiring a Vulkan device, while real backends can own receiver-side
    // Vulkan dispatch tables and source-to-destination handle maps.
    void set_executor(std::unique_ptr<ReplayExecutor> executor);

    // receive() owns the receiver-side replay boundary. Transport and schema
    // decoding live below TransportSession, so the framework only sees captured
    // calls.
    void receive(const InterceptedCall & call);

private:
    ReceiverSession & session_;

    std::unique_ptr<ReplayExecutor> executor_;
};

} // namespace vkfwd
