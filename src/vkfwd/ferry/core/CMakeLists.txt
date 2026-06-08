# vkfwd_ferry_core holds the shared command model, generated pack/unpack code,
# transport contracts, protocol metadata, and small debug implementations. The
# Vulkan layer and receiver link this target so generated payload behavior stays
# identical on both sides of the forwarding boundary.
add_library(vkfwd_ferry_core STATIC
  command_stream.cpp
  memory_map/manager.cpp
  memory_map/memory_info_fallback.cpp
  memory_map/memory_type_registry.cpp
  memory_map/forwarder_allocation.cpp
  memory_map/forwarder/non_coherent_allocation.cpp
  memory_map/forwarder/coherent_allocation.cpp
  memory_map/forwarder_allocation_factory.cpp
  memory_map/receiver_allocation.cpp
  memory_map/receiver/non_coherent_allocation.cpp
  memory_map/receiver/coherent_allocation.cpp
  memory_map/receiver_allocation_factory.cpp
  memory_map/vm_primitives_linux.cpp
  memory_map/vm_primitives_macos.cpp
  memory_map/vm_primitives_windows.cpp
  generated/command/vkAllocateMemory.cpp
  generated/command/vkBindBufferMemory.cpp
  generated/command/vkCreateBuffer.cpp
  generated/command/vkCreateDescriptorSetLayout.cpp
  generated/command/vkEnumerateInstanceVersion.cpp
  generated/command/vkEnumerateInstanceLayerProperties.cpp
  generated/command/vkEnumerateInstanceExtensionProperties.cpp
  generated/command/vkCreateGraphicsPipelines.cpp
  generated/command/vkCreateDevice.cpp
  generated/command/vkCreateInstance.cpp
  generated/command/vkCreatePipelineLayout.cpp
  generated/command/vkCreateRenderPass.cpp
  generated/command/vkCreateSemaphore.cpp
  generated/command/vkCreateShaderModule.cpp
  generated/command/vkDestroyBuffer.cpp
  generated/command/vkDestroyDescriptorSetLayout.cpp
  generated/command/vkDestroyDevice.cpp
  generated/command/vkDestroyInstance.cpp
  generated/command/vkDestroyPipeline.cpp
  generated/command/vkDestroyPipelineLayout.cpp
  generated/command/vkDestroyRenderPass.cpp
  generated/command/vkDestroySemaphore.cpp
  generated/command/vkDestroyShaderModule.cpp
  generated/command/vkDeviceWaitIdle.cpp
  generated/command/vkEnumerateDeviceExtensionProperties.cpp
  generated/command/vkEnumeratePhysicalDevices.cpp
  generated/command/vkFreeMemory.cpp
  generated/command/vkGetBufferMemoryRequirements.cpp
  generated/command/vkGetDeviceQueue.cpp
  generated/command/vkGetPhysicalDeviceFeatures.cpp
  generated/command/vkGetPhysicalDeviceMemoryProperties.cpp
  generated/command/vkGetPhysicalDeviceProperties.cpp
  generated/command/vkGetPhysicalDeviceQueueFamilyProperties.cpp
  generated/command/vkMapMemory.cpp
  generated/command/vkUnmapMemory.cpp
  generated/dispatch_table.cpp
  generated/structure/command_structs.cpp
  generated/structure/core.cpp
  hook/vkCreateDeviceHook.cpp
  logging.cpp)

set_target_properties(vkfwd_ferry_core PROPERTIES
  # Core is linked into the Vulkan layer shared object, so it must be
  # position-independent even though it is currently built as static.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_ferry_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    # Per-strategy ForwarderAllocation subclasses (memory_map/forwarder/) push
    # manual command chunks through Forwarder::instance(); the symbol resolves
    # at final link via vkfwd_ferry_forwarder_static. Receiver-only consumers
    # never reach those subclasses through the factory chain, so the unresolved
    # reference is harmless for them.
    ${CMAKE_CURRENT_SOURCE_DIR}/../forwarder
    # Per-strategy ReceiverAllocation subclasses (memory_map/receiver/) need
    # ReplayContext (source->receiver handle maps + per-context dispatch table)
    # to translate handles and call the local Vulkan driver at endpoint time.
    # That header lives in the receiver tree to stay co-located with the
    # receiver-only state it owns. Mirrors the forwarder include above;
    # forwarder-only consumers never link a path that references these symbols.
    ${CMAKE_CURRENT_SOURCE_DIR}/../receiver)

target_link_libraries(vkfwd_ferry_core
  PUBLIC
    vkfwd_vulkan_headers
    fmt::fmt
    spdlog::spdlog)

if(UNIX AND NOT APPLE)
  target_link_libraries(vkfwd_ferry_core
    PUBLIC
      dl)
endif()

target_compile_definitions(vkfwd_ferry_core
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")
