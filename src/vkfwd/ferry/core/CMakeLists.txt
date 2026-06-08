# vkfwd_ferry_core holds the shared command model, generated pack/unpack code,
# transport contracts, protocol metadata, and small debug implementations. The
# Vulkan layer and receiver link this target so generated payload behavior stays
# identical on both sides of the forwarding boundary.
add_library(vkfwd_ferry_core STATIC
  command_stream.cpp
  memory_map_manager.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR})

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
