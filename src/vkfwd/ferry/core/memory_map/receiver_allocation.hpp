#pragma once

#include "command_stream.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace vkfwd::receiver {
struct ReplayContext;
}

namespace vkfwd::memory_map {

// Per-VkDeviceMemory polymorphic handler on the receiver side. One
// concrete subclass per allocation, owned by MemoryMapReceiver's
// per-handle map. The receiver mapped pointer (if any) stays private to
// the subclass and must never escape to source-process state.
class ReceiverAllocation {
public:
    struct CreationInfo {
        // Source-visible VkDevice (also the per-context dispatch key); the
        // receiver-native VkDevice that real Vulkan calls need is obtained
        // via ReplayContext's source→receiver handle map at call sites,
        // matching how every other receiver endpoint performs translation.
        VkDevice device;
        // Source-visible VkDeviceMemory; this same value is the
        // MemoryMapReceiver per-handle map key and matches the handle that
        // arrives in wire payloads. The receiver-native VkDeviceMemory is
        // also obtained via ReplayContext's handle map at call sites. The
        // handle-map dependency is a phase-1 prerequisite (see "What's
        // Deferred" in doc/memory_map_management.md).
        VkDeviceMemory        memory;
        VkDeviceSize          allocation_size;
        std::uint32_t         memory_type_index;
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        // Mirrors the forwarder-side alignment carry so receiver-side
        // validation against the *ppData - offset contract can use the same
        // value without re-resolving via registry.
        std::size_t min_memory_map_alignment;
    };

    virtual ~ReceiverAllocation() = default;

    virtual bool map_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                              ::vkfwd::receiver::ReplayContext & replay_context)        = 0;
    virtual bool unmap_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                ::vkfwd::receiver::ReplayContext & replay_context)      = 0;
    virtual bool flush_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                ::vkfwd::receiver::ReplayContext & replay_context)      = 0;
    virtual bool invalidate_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                     ::vkfwd::receiver::ReplayContext & replay_context) = 0;

    const CreationInfo & info() const { return info_; }

    // Same rationale as ForwarderAllocation: exposed so make_unique can
    // construct subclasses through the inherited-constructor pattern. Class
    // is abstract via pure virtuals.
    explicit ReceiverAllocation(const CreationInfo & info): info_(info) {}

private:
    CreationInfo info_;
};

} // namespace vkfwd::memory_map
