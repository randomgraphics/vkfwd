#include "manual_dispatch.hpp"

#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver {

bool dispatch_manual_command(::vkfwd::manual::CommandId command_id, const CommandStream & request_stream, const Range & request_range,
                             CommandStream & response_stream, ReplayContext & replay_context) {
    switch (command_id) {
    case ::vkfwd::manual::CommandId::MemoryMap:
        return replay_context.memoryMap.custom_vkMapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::MemoryUnmap:
        return replay_context.memoryMap.custom_vkUnmapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo:
        // Implementation lands in Task 13 of this plan.
        VKFWD_LOG_ERROR("vkfwd receiver: manual::CommandId::QueryPhysicalDeviceMemoryInfo not yet wired");
        return false;
    }
    VKFWD_LOG_ERROR("vkfwd receiver: unknown manual command_id={}", static_cast<std::uint32_t>(command_id));
    return false;
}

} // namespace vkfwd::receiver
