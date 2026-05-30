#include "generated/command/vkCreateDevice.hpp"

#include "logging.hpp"

namespace vkfwd::manual {

void CommandHooks<vkfwd::generated::CommandId::CreateDevice>::before_pack(Parameters &) {
    // This intentionally lives outside generated code to prove command-specific
    // Vulkan capture policy can be owned by humans without regeneration touching
    // the hook body.
    VKFWD_LOG_DEBUG("vkfwd hook: before packing vkCreateDevice");
}

} // namespace vkfwd::manual
