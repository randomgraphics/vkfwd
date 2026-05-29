# Vulkan Return And Output API Groups

This document groups Vulkan commands that either return a value or write caller
visible output parameters. These commands are critical for `vkfwd` because they
cannot be handled as simple fire-and-forget forwarding records: the intercepted
entry point must produce the exact return value, output parameter contents, and
new handle identities expected by the application before it returns.

Commands with neither a return value nor output parameters are still important
for replay ordering, but they have a simpler completion contract. They can often
be serialized more cheaply because the caller does not need synchronous result
data from the receiver. The APIs listed here are the expensive boundary: they
drive synchronous forwarding, output payload ownership, two-call query handling,
source-to-receiver handle mapping, and cache policy decisions.

Source registry:

```text
src/third_party/vulkan/registry/vk.xml
```

The generated coverage file currently identifies this registry as Vulkan API
version 1.4.352 with header version 352.

## Classification Rules

- Aliases are omitted.
- Duplicate command definitions are collapsed by command name.
- A command is included when its return type is not `void` or when it has at
  least one output parameter.
- Output parameters are detected from non-const pointer parameters.
- Known native WSI input pointers, such as `Display* dpy`,
  `wl_display* display`, and `xcb_connection_t* connection`, are not treated as
  output parameters.
- A command is assigned to the first matching operation category. For example,
  [`vkCreatePipelineCache`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreatePipelineCache.html) is grouped under handle/object creation because its
  caller-visible contract is creating and returning a handle.

This grouping is intentionally operation-oriented instead of extension-oriented.
The implementation needs to know how a command behaves at the interception
boundary: create a handle, map memory, enumerate a list, fill an output buffer,
return a status, or expose an external platform object.

## Implementation Impact

Handle/object creation APIs must return source-side handle values that the
application can use immediately while preserving a mapping to receiver-side
handles. Replay cannot reuse raw driver handles from the source process.

Map/unmap APIs expose pointer lifetime and memory visibility constraints.
[`vkMapMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory.html) and [`vkMapMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory2.html) return process-local pointers, so forwarding
must define whether mapped memory is mirrored, proxied, copied, or handled by a
manual path. The returned pointer is not a replay-stable value.

Enumeration and two-call queries often require a count-only call followed by a
data-fill call. Efficient handling should avoid unnecessary receiver round trips
where results can be cached safely, but it must preserve Vulkan's count,
`VK_INCOMPLETE`, and output-array behavior.

Property, feature, capability, and support queries are often read-only and
cacheable. They still affect correctness because applications make creation and
rendering decisions from these outputs. Cache keys must include every input that
can affect the result, including `pNext` chains where applicable.

Data readback and binary/blob APIs can move large payloads. These need explicit
ownership rules for copied output bytes and should avoid heap churn in common
small-result paths.

Synchronization, wait, status, and submit APIs may have no output parameters but
return meaningful completion state. They cannot be blindly acknowledged without
matching Vulkan ordering and synchronization semantics.

External-handle and WSI APIs cross process, OS, display-server, or window-system
boundaries. They often require platform-specific policy rather than generic
payload replay.

## High-Frequency Call Dimension

The operation categories below describe what a command does. Frequency is a
separate implementation dimension: some result-producing APIs may still appear
inside per-frame or per-draw hot paths, while many other result-producing APIs
are setup-time queries. Hot-path commands need low-overhead generated handling,
careful cache policy, and minimal synchronous receiver traffic.

High-frequency commands with return values or output parameters are the most
likely performance killers for `vkfwd`. They combine repeated per-frame use with
the hardest forwarding contract: the intercepted call cannot return until the
caller-visible result is known and output storage has been filled. These commands
deserve special attention before broad API coverage is considered complete,
because a correct but round-trip-heavy implementation can dominate frame time.

This list is intentionally conservative. It identifies commands from this
return/output inventory that engines may call many times within a frame, may
poll repeatedly, or may place near command-buffer construction. Classic draw and
dispatch commands such as [`vkCmdDraw`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdDraw.html) are not listed because they have no return
value and no output parameters, so they are outside this document's inventory.

| Frequency tier | Commands | Implementation concern |
| --- | --- | --- |
| Per command-buffer recording or per draw-marker path | [`vkBeginCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBeginCommandBuffer.html), [`vkEndCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEndCommandBuffer.html), [`vkResetCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetCommandBuffer.html), [`vkCmdSetPerformanceMarkerINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceMarkerINTEL.html), [`vkCmdSetPerformanceStreamMarkerINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceStreamMarkerINTEL.html), [`vkCmdSetPerformanceOverrideINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceOverrideINTEL.html), [`vkCmdBeginGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdBeginGpaSessionAMD.html), [`vkCmdEndGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdEndGpaSessionAMD.html), [`vkCmdBeginGpaSampleAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdBeginGpaSampleAMD.html) | These sit near command recording. Even when rarely enabled, the generated path should avoid dynamic lookup, allocation, and avoidable branches. |
| Descriptor-buffer and descriptor helper hot path | [`vkGetDescriptorEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorEXT.html), [`vkWriteSamplerDescriptorsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteSamplerDescriptorsEXT.html), [`vkWriteResourceDescriptorsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteResourceDescriptorsEXT.html), [`vkGetDescriptorSetLayoutSizeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutSizeEXT.html), [`vkGetDescriptorSetLayoutBindingOffsetEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutBindingOffsetEXT.html), [`vkGetDescriptorSetHostMappingVALVE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetHostMappingVALVE.html), [`vkGetDescriptorSetLayoutHostMappingInfoVALVE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutHostMappingInfoVALVE.html) | Descriptor data can be produced while building draw state. Returned host pointers or descriptor bytes need command-specific ownership rules. |
| Dynamic upload, mapped-memory, and visibility path | [`vkMapMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory.html), [`vkMapMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory2.html), [`vkUnmapMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkUnmapMemory2.html), [`vkFlushMappedMemoryRanges`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkFlushMappedMemoryRanges.html), [`vkInvalidateMappedMemoryRanges`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkInvalidateMappedMemoryRanges.html) | Engines that map or flush per update can make these expensive. Returned mapped pointers are source-process local and cannot be replayed as raw pointer values. |
| Synchronization polling and frame pacing | [`vkGetFenceStatus`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceStatus.html), [`vkWaitForFences`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForFences.html), [`vkResetFences`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetFences.html), [`vkGetEventStatus`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetEventStatus.html), [`vkSetEvent`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetEvent.html), [`vkResetEvent`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetEvent.html), [`vkWaitSemaphores`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitSemaphores.html), [`vkSignalSemaphore`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSignalSemaphore.html), [`vkGetSemaphoreCounterValue`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreCounterValue.html), [`vkQueueSubmit`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSubmit.html), [`vkQueueSubmit2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSubmit2.html), [`vkQueueWaitIdle`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueWaitIdle.html), [`vkDeviceWaitIdle`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDeviceWaitIdle.html) | Polling and waits expose real progress. Fake success can break ordering; round trips can stall the CPU side of the frame. |
| Query and instrumentation result readback | [`vkGetQueryPoolResults`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueryPoolResults.html), [`vkGetQueueCheckpointDataNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueueCheckpointDataNV.html), [`vkGetQueueCheckpointData2NV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueueCheckpointData2NV.html), [`vkGetPipelineExecutableStatisticsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineExecutableStatisticsKHR.html), [`vkGetPipelineExecutableInternalRepresentationsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineExecutableInternalRepresentationsKHR.html), [`vkGetGpaSessionStatusAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGpaSessionStatusAMD.html), [`vkGetGpaSessionResultsAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGpaSessionResultsAMD.html) | Result buffers may be read every frame for timestamps, occlusion, profiling, or diagnostics. Avoid heap churn and preserve partial-result semantics. |
| Address, opaque handle, and descriptor-capture scalar queries | [`vkGetBufferDeviceAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferDeviceAddress.html), [`vkGetBufferOpaqueCaptureAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferOpaqueCaptureAddress.html), [`vkGetDeviceMemoryOpaqueCaptureAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceMemoryOpaqueCaptureAddress.html), [`vkGetAccelerationStructureDeviceAddressKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureDeviceAddressKHR.html), [`vkGetPipelineIndirectDeviceAddressNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineIndirectDeviceAddressNV.html), [`vkGetImageViewHandleNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewHandleNVX.html), [`vkGetImageViewHandle64NVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewHandle64NVX.html), [`vkGetDeviceCombinedImageSamplerIndexNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceCombinedImageSamplerIndexNVX.html) | These are cheap scalar returns but may be called while building GPU-visible data. Source scalar values may not be valid receiver-side values. |
| Per-frame WSI and presentation | [`vkAcquireNextImageKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireNextImageKHR.html), [`vkAcquireNextImage2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireNextImage2KHR.html), [`vkQueuePresentKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueuePresentKHR.html), [`vkGetSwapchainStatusKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainStatusKHR.html), [`vkWaitForPresentKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForPresentKHR.html), [`vkWaitForPresent2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForPresent2KHR.html), [`vkReleaseSwapchainImagesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleaseSwapchainImagesKHR.html), [`vkGetPastPresentationTimingGOOGLE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPastPresentationTimingGOOGLE.html), [`vkGetPastPresentationTimingEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPastPresentationTimingEXT.html), [`vkGetRefreshCycleDurationGOOGLE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRefreshCycleDurationGOOGLE.html), [`vkGetSwapchainCounterEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainCounterEXT.html) | Usually frame-rate rather than draw-rate, but these are latency-sensitive and directly affect image ownership and pacing. |
| Repeated capability or requirement queries seen in some engines | [`vkGetBufferMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferMemoryRequirements.html), [`vkGetBufferMemoryRequirements2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferMemoryRequirements2.html), [`vkGetImageMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageMemoryRequirements.html), [`vkGetImageMemoryRequirements2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageMemoryRequirements2.html), [`vkGetImageSubresourceLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSubresourceLayout.html), [`vkGetImageSubresourceLayout2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSubresourceLayout2.html), [`vkGetPhysicalDeviceFormatProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFormatProperties.html), [`vkGetPhysicalDeviceFormatProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFormatProperties2.html), [`vkGetPhysicalDeviceImageFormatProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceImageFormatProperties.html), [`vkGetPhysicalDeviceImageFormatProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceImageFormatProperties2.html) | These should be good cache candidates when the cache key includes all relevant inputs and `pNext` result shape. |

## Category Summary

| Operation category | Count |
| --- | ---: |
| Loader and dispatch function lookup | 2 |
| Handle/object creation, allocation, acquisition, and registration | 86 |
| Memory map and unmap | 3 |
| Memory allocation, binding, requirements, and residency/commitment | 31 |
| External handle import/export and platform interop | 38 |
| WSI surface, display, present, swapchain, and image acquire | 60 |
| Enumeration and two-call count/list queries | 36 |
| Data readback, binary/blob extraction, and result buffers | 38 |
| Device/opaque address and small scalar handle queries | 7 |
| Synchronization, queue submission, waits, status, and command lifecycle returns | 51 |
| Property, feature, capability, support, compatibility, size, and layout queries | 46 |
| Descriptor write/query helper commands | 3 |
| Pipeline, shader, ray tracing, acceleration-structure, and micromap operations | 3 |
| Video, tensor, data-graph, optical-flow, and vendor compute features | 3 |
| Debug, validation, fault, private-data, profiling, and latency control | 7 |
| **Total** | **414** |

## Loader And Dispatch Function Lookup

These functions are loader-chain critical. They affect which function pointer
the application calls next, so interception must preserve the Vulkan loader and
dispatch-table invariants instead of treating them as ordinary replay payloads.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetDeviceProcAddr`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceProcAddr.html) | `PFN_vkVoidFunction` | - |
| [`vkGetInstanceProcAddr`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetInstanceProcAddr.html) | `PFN_vkVoidFunction` | - |

## Handle/Object Creation, Allocation, Acquisition, And Registration

These APIs create or acquire caller-visible handles or handle-like identities.
The source process receives a value immediately, while replay uses a separate
receiver-side handle. The implementation must update the source-to-receiver
handle map only when the call succeeds according to that command's return rules.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkCreateInstance`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateInstance.html) | `VkResult` | `pInstance` |
| [`vkCreateDevice`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDevice.html) | `VkResult` | `pDevice` |
| [`vkGetDeviceQueue`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceQueue.html) | `void` | `pQueue` |
| [`vkAllocateMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAllocateMemory.html) | `VkResult` | `pMemory` |
| [`vkCreateFence`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateFence.html) | `VkResult` | `pFence` |
| [`vkCreateSemaphore`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSemaphore.html) | `VkResult` | `pSemaphore` |
| [`vkCreateEvent`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateEvent.html) | `VkResult` | `pEvent` |
| [`vkCreateQueryPool`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateQueryPool.html) | `VkResult` | `pQueryPool` |
| [`vkCreateBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateBuffer.html) | `VkResult` | `pBuffer` |
| [`vkCreateBufferView`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateBufferView.html) | `VkResult` | `pView` |
| [`vkCreateImage`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateImage.html) | `VkResult` | `pImage` |
| [`vkCreateImageView`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateImageView.html) | `VkResult` | `pView` |
| [`vkCreateShaderModule`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateShaderModule.html) | `VkResult` | `pShaderModule` |
| [`vkCreatePipelineCache`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreatePipelineCache.html) | `VkResult` | `pPipelineCache` |
| [`vkCreatePipelineBinariesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreatePipelineBinariesKHR.html) | `VkResult` | `pBinaries` |
| [`vkCreateGraphicsPipelines`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateGraphicsPipelines.html) | `VkResult` | `pPipelines` |
| [`vkCreateComputePipelines`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateComputePipelines.html) | `VkResult` | `pPipelines` |
| [`vkCreatePipelineLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreatePipelineLayout.html) | `VkResult` | `pPipelineLayout` |
| [`vkCreateSampler`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSampler.html) | `VkResult` | `pSampler` |
| [`vkCreateDescriptorSetLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDescriptorSetLayout.html) | `VkResult` | `pSetLayout` |
| [`vkCreateDescriptorPool`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDescriptorPool.html) | `VkResult` | `pDescriptorPool` |
| [`vkAllocateDescriptorSets`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAllocateDescriptorSets.html) | `VkResult` | `pDescriptorSets` |
| [`vkCreateFramebuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateFramebuffer.html) | `VkResult` | `pFramebuffer` |
| [`vkCreateRenderPass`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateRenderPass.html) | `VkResult` | `pRenderPass` |
| [`vkCreateCommandPool`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateCommandPool.html) | `VkResult` | `pCommandPool` |
| [`vkAllocateCommandBuffers`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAllocateCommandBuffers.html) | `VkResult` | `pCommandBuffers` |
| [`vkCreateAndroidSurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateAndroidSurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateSurfaceOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSurfaceOHOS.html) | `VkResult` | `pSurface` |
| [`vkCreateDisplayModeKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDisplayModeKHR.html) | `VkResult` | `pMode` |
| [`vkCreateDisplayPlaneSurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDisplayPlaneSurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateSharedSwapchainsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSharedSwapchainsKHR.html) | `VkResult` | `pSwapchains` |
| [`vkCreateSwapchainKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSwapchainKHR.html) | `VkResult` | `pSwapchain` |
| [`vkCreateViSurfaceNN`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateViSurfaceNN.html) | `VkResult` | `pSurface` |
| [`vkCreateWaylandSurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateWaylandSurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateUbmSurfaceSEC`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateUbmSurfaceSEC.html) | `VkResult` | `pSurface` |
| [`vkCreateWin32SurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateWin32SurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateXlibSurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateXlibSurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateXcbSurfaceKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateXcbSurfaceKHR.html) | `VkResult` | `pSurface` |
| [`vkCreateDirectFBSurfaceEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDirectFBSurfaceEXT.html) | `VkResult` | `pSurface` |
| [`vkCreateImagePipeSurfaceFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateImagePipeSurfaceFUCHSIA.html) | `VkResult` | `pSurface` |
| [`vkCreateStreamDescriptorSurfaceGGP`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateStreamDescriptorSurfaceGGP.html) | `VkResult` | `pSurface` |
| [`vkCreateScreenSurfaceQNX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateScreenSurfaceQNX.html) | `VkResult` | `pSurface` |
| [`vkCreateDebugReportCallbackEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDebugReportCallbackEXT.html) | `VkResult` | `pCallback` |
| [`vkCreateIndirectCommandsLayoutNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateIndirectCommandsLayoutNV.html) | `VkResult` | `pIndirectCommandsLayout` |
| [`vkCreateIndirectCommandsLayoutEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateIndirectCommandsLayoutEXT.html) | `VkResult` | `pIndirectCommandsLayout` |
| [`vkCreateIndirectExecutionSetEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateIndirectExecutionSetEXT.html) | `VkResult` | `pIndirectExecutionSet` |
| [`vkCreateSemaphoreSciSyncPoolNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSemaphoreSciSyncPoolNV.html) | `VkResult` | `pSemaphorePool` |
| [`vkRegisterDeviceEventEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkRegisterDeviceEventEXT.html) | `VkResult` | `pFence` |
| [`vkRegisterDisplayEventEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkRegisterDisplayEventEXT.html) | `VkResult` | `pFence` |
| [`vkCreateDescriptorUpdateTemplate`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDescriptorUpdateTemplate.html) | `VkResult` | `pDescriptorUpdateTemplate` |
| [`vkCreateIOSSurfaceMVK`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateIOSSurfaceMVK.html) | `VkResult` | `pSurface` |
| [`vkCreateMacOSSurfaceMVK`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateMacOSSurfaceMVK.html) | `VkResult` | `pSurface` |
| [`vkCreateMetalSurfaceEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateMetalSurfaceEXT.html) | `VkResult` | `pSurface` |
| [`vkCreateSamplerYcbcrConversion`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateSamplerYcbcrConversion.html) | `VkResult` | `pYcbcrConversion` |
| [`vkGetDeviceQueue2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceQueue2.html) | `void` | `pQueue` |
| [`vkCreateValidationCacheEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateValidationCacheEXT.html) | `VkResult` | `pValidationCache` |
| [`vkCreateDebugUtilsMessengerEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDebugUtilsMessengerEXT.html) | `VkResult` | `pMessenger` |
| [`vkCreateRenderPass2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateRenderPass2.html) | `VkResult` | `pRenderPass` |
| [`vkCreateAccelerationStructureNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateAccelerationStructureNV.html) | `VkResult` | `pAccelerationStructure` |
| [`vkCreateRayTracingPipelinesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateRayTracingPipelinesNV.html) | `VkResult` | `pPipelines` |
| [`vkCreateRayTracingPipelinesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateRayTracingPipelinesKHR.html) | `VkResult` | `pPipelines` |
| [`vkCreateHeadlessSurfaceEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateHeadlessSurfaceEXT.html) | `VkResult` | `pSurface` |
| [`vkAcquirePerformanceConfigurationINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquirePerformanceConfigurationINTEL.html) | `VkResult` | `pConfiguration` |
| [`vkCreateAccelerationStructureKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateAccelerationStructureKHR.html) | `VkResult` | `pAccelerationStructure` |
| [`vkCreateDeferredOperationKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDeferredOperationKHR.html) | `VkResult` | `pDeferredOperation` |
| [`vkCreatePrivateDataSlot`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreatePrivateDataSlot.html) | `VkResult` | `pPrivateDataSlot` |
| [`vkCreateVideoSessionKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateVideoSessionKHR.html) | `VkResult` | `pVideoSession` |
| [`vkCreateVideoSessionParametersKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateVideoSessionParametersKHR.html) | `VkResult` | `pVideoSessionParameters` |
| [`vkCreateCuModuleNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateCuModuleNVX.html) | `VkResult` | `pModule` |
| [`vkCreateCuFunctionNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateCuFunctionNVX.html) | `VkResult` | `pFunction` |
| [`vkCreateBufferCollectionFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateBufferCollectionFUCHSIA.html) | `VkResult` | `pCollection` |
| [`vkCreateCudaModuleNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateCudaModuleNV.html) | `VkResult` | `pModule` |
| [`vkCreateCudaFunctionNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateCudaFunctionNV.html) | `VkResult` | `pFunction` |
| [`vkCreateMicromapEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateMicromapEXT.html) | `VkResult` | `pMicromap` |
| [`vkCreateOpticalFlowSessionNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateOpticalFlowSessionNV.html) | `VkResult` | `pSession` |
| [`vkCreateShadersEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateShadersEXT.html) | `VkResult` | `pShaders` |
| [`vkCreateExecutionGraphPipelinesAMDX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateExecutionGraphPipelinesAMDX.html) | `VkResult` | `pPipelines` |
| [`vkCreateGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateGpaSessionAMD.html) | `VkResult` | `pGpaSession` |
| [`vkCreateExternalComputeQueueNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateExternalComputeQueueNV.html) | `VkResult` | `pExternalQueue` |
| [`vkCreateShaderInstrumentationARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateShaderInstrumentationARM.html) | `VkResult` | `pInstrumentation` |
| [`vkCreateTensorARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateTensorARM.html) | `VkResult` | `pTensor` |
| [`vkCreateTensorViewARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateTensorViewARM.html) | `VkResult` | `pView` |
| [`vkCreateDataGraphPipelinesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDataGraphPipelinesARM.html) | `VkResult` | `pPipelines` |
| [`vkCreateDataGraphPipelineSessionARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateDataGraphPipelineSessionARM.html) | `VkResult` | `pSession` |
| [`vkRegisterCustomBorderColorEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkRegisterCustomBorderColorEXT.html) | `VkResult` | `pIndex` |
| [`vkCreateAccelerationStructure2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCreateAccelerationStructure2KHR.html) | `VkResult` | `pAccelerationStructure` |

## Memory Map And Unmap

Mapped pointers are process-local and lifetime-sensitive. These APIs need an
explicit forwarding policy because serializing the returned pointer value itself
does not make memory visible in the receiver process.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkMapMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory.html) | `VkResult` | `ppData` |
| [`vkMapMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMapMemory2.html) | `VkResult` | `ppData` |
| [`vkUnmapMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkUnmapMemory2.html) | `VkResult` | - |

## Memory Allocation, Binding, Requirements, And Residency/Commitment

These APIs affect memory ownership, compatibility, and binding state. Requirement
queries may be cacheable, but allocation and binding calls change replay state
and must preserve ordering against resource creation and queue work.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetPhysicalDeviceMemoryProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceMemoryProperties.html) | `void` | `pMemoryProperties` |
| [`vkFlushMappedMemoryRanges`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkFlushMappedMemoryRanges.html) | `VkResult` | - |
| [`vkInvalidateMappedMemoryRanges`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkInvalidateMappedMemoryRanges.html) | `VkResult` | - |
| [`vkGetDeviceMemoryCommitment`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceMemoryCommitment.html) | `void` | `pCommittedMemoryInBytes` |
| [`vkGetBufferMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferMemoryRequirements.html) | `void` | `pMemoryRequirements` |
| [`vkBindBufferMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindBufferMemory.html) | `VkResult` | - |
| [`vkGetImageMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageMemoryRequirements.html) | `void` | `pMemoryRequirements` |
| [`vkBindImageMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindImageMemory.html) | `VkResult` | - |
| [`vkGetImageSparseMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSparseMemoryRequirements.html) | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| [`vkGetGeneratedCommandsMemoryRequirementsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGeneratedCommandsMemoryRequirementsNV.html) | `void` | `pMemoryRequirements` |
| [`vkGetGeneratedCommandsMemoryRequirementsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGeneratedCommandsMemoryRequirementsEXT.html) | `void` | `pMemoryRequirements` |
| [`vkGetPhysicalDeviceMemoryProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceMemoryProperties2.html) | `void` | `pMemoryProperties` |
| [`vkBindBufferMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindBufferMemory2.html) | `VkResult` | - |
| [`vkBindImageMemory2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindImageMemory2.html) | `VkResult` | - |
| [`vkGetBufferMemoryRequirements2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferMemoryRequirements2.html) | `void` | `pMemoryRequirements` |
| [`vkGetImageMemoryRequirements2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageMemoryRequirements2.html) | `void` | `pMemoryRequirements` |
| [`vkGetImageSparseMemoryRequirements2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSparseMemoryRequirements2.html) | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| [`vkGetDeviceBufferMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceBufferMemoryRequirements.html) | `void` | `pMemoryRequirements` |
| [`vkGetDeviceImageMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceImageMemoryRequirements.html) | `void` | `pMemoryRequirements` |
| [`vkGetDeviceImageSparseMemoryRequirements`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceImageSparseMemoryRequirements.html) | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| [`vkGetAccelerationStructureMemoryRequirementsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureMemoryRequirementsNV.html) | `void` | `pMemoryRequirements` |
| [`vkGetClusterAccelerationStructureBuildSizesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetClusterAccelerationStructureBuildSizesNV.html) | `void` | `pSizeInfo` |
| [`vkGetPipelineIndirectMemoryRequirementsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineIndirectMemoryRequirementsNV.html) | `void` | `pMemoryRequirements` |
| [`vkGetAccelerationStructureBuildSizesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureBuildSizesKHR.html) | `void` | `pSizeInfo` |
| [`vkGetCommandPoolMemoryConsumption`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetCommandPoolMemoryConsumption.html) | `void` | `pConsumption` |
| [`vkGetVideoSessionMemoryRequirementsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetVideoSessionMemoryRequirementsKHR.html) | `VkResult` | `pMemoryRequirementsCount`, `pMemoryRequirements` |
| [`vkGetPartitionedAccelerationStructuresBuildSizesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPartitionedAccelerationStructuresBuildSizesNV.html) | `void` | `pSizeInfo` |
| [`vkGetMicromapBuildSizesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMicromapBuildSizesEXT.html) | `void` | `pSizeInfo` |
| [`vkGetTensorMemoryRequirementsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetTensorMemoryRequirementsARM.html) | `void` | `pMemoryRequirements` |
| [`vkGetDeviceTensorMemoryRequirementsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceTensorMemoryRequirementsARM.html) | `void` | `pMemoryRequirements` |
| [`vkGetDataGraphPipelineSessionMemoryRequirementsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDataGraphPipelineSessionMemoryRequirementsARM.html) | `void` | `pMemoryRequirements` |

## External Handle Import/Export And Platform Interop

These commands cross OS and process boundaries. Generic replay is usually not
enough; the implementation must define ownership and transfer behavior for file
descriptors, Win32 handles, Zircon handles, Android buffers, Metal objects, and
similar platform resources.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetMemoryWin32HandleNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryWin32HandleNV.html) | `VkResult` | `pHandle` |
| [`vkGetMemoryWin32HandleKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryWin32HandleKHR.html) | `VkResult` | `pHandle` |
| [`vkGetMemoryWin32HandlePropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryWin32HandlePropertiesKHR.html) | `VkResult` | `pMemoryWin32HandleProperties` |
| [`vkGetMemoryFdKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryFdKHR.html) | `VkResult` | `pFd` |
| [`vkGetMemoryFdPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryFdPropertiesKHR.html) | `VkResult` | `pMemoryFdProperties` |
| [`vkGetMemoryZirconHandleFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryZirconHandleFUCHSIA.html) | `VkResult` | `pZirconHandle` |
| [`vkGetMemoryZirconHandlePropertiesFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryZirconHandlePropertiesFUCHSIA.html) | `VkResult` | `pMemoryZirconHandleProperties` |
| [`vkGetMemoryRemoteAddressNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryRemoteAddressNV.html) | `VkResult` | `pAddress` |
| [`vkGetMemorySciBufNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemorySciBufNV.html) | `VkResult` | `pHandle` |
| [`vkGetPhysicalDeviceExternalMemorySciBufPropertiesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalMemorySciBufPropertiesNV.html) | `VkResult` | `pMemorySciBufProperties` |
| [`vkGetPhysicalDeviceSciBufAttributesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSciBufAttributesNV.html) | `VkResult` | - |
| [`vkGetSemaphoreWin32HandleKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreWin32HandleKHR.html) | `VkResult` | `pHandle` |
| [`vkImportSemaphoreWin32HandleKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportSemaphoreWin32HandleKHR.html) | `VkResult` | - |
| [`vkGetSemaphoreFdKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreFdKHR.html) | `VkResult` | `pFd` |
| [`vkImportSemaphoreFdKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportSemaphoreFdKHR.html) | `VkResult` | - |
| [`vkGetSemaphoreZirconHandleFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreZirconHandleFUCHSIA.html) | `VkResult` | `pZirconHandle` |
| [`vkImportSemaphoreZirconHandleFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportSemaphoreZirconHandleFUCHSIA.html) | `VkResult` | - |
| [`vkGetFenceWin32HandleKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceWin32HandleKHR.html) | `VkResult` | `pHandle` |
| [`vkImportFenceWin32HandleKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportFenceWin32HandleKHR.html) | `VkResult` | - |
| [`vkGetFenceFdKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceFdKHR.html) | `VkResult` | `pFd` |
| [`vkImportFenceFdKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportFenceFdKHR.html) | `VkResult` | - |
| [`vkGetFenceSciSyncFenceNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceSciSyncFenceNV.html) | `VkResult` | `pHandle` |
| [`vkGetFenceSciSyncObjNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceSciSyncObjNV.html) | `VkResult` | `pHandle` |
| [`vkImportFenceSciSyncFenceNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportFenceSciSyncFenceNV.html) | `VkResult` | - |
| [`vkImportFenceSciSyncObjNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportFenceSciSyncObjNV.html) | `VkResult` | - |
| [`vkGetSemaphoreSciSyncObjNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreSciSyncObjNV.html) | `VkResult` | `pHandle` |
| [`vkImportSemaphoreSciSyncObjNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkImportSemaphoreSciSyncObjNV.html) | `VkResult` | - |
| [`vkGetPhysicalDeviceSciSyncAttributesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSciSyncAttributesNV.html) | `VkResult` | - |
| [`vkGetAndroidHardwareBufferPropertiesANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAndroidHardwareBufferPropertiesANDROID.html) | `VkResult` | `pProperties` |
| [`vkGetMemoryAndroidHardwareBufferANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryAndroidHardwareBufferANDROID.html) | `VkResult` | `pBuffer` |
| [`vkSetBufferCollectionBufferConstraintsFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetBufferCollectionBufferConstraintsFUCHSIA.html) | `VkResult` | - |
| [`vkSetBufferCollectionImageConstraintsFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetBufferCollectionImageConstraintsFUCHSIA.html) | `VkResult` | - |
| [`vkGetBufferCollectionPropertiesFUCHSIA`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferCollectionPropertiesFUCHSIA.html) | `VkResult` | `pProperties` |
| [`vkExportMetalObjectsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkExportMetalObjectsEXT.html) | `void` | `pMetalObjectsInfo` |
| [`vkGetMemoryMetalHandleEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryMetalHandleEXT.html) | `VkResult` | `pHandle` |
| [`vkGetMemoryMetalHandlePropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryMetalHandlePropertiesEXT.html) | `VkResult` | `pMemoryMetalHandleProperties` |
| [`vkGetNativeBufferPropertiesOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetNativeBufferPropertiesOHOS.html) | `VkResult` | `pProperties` |
| [`vkGetMemoryNativeBufferOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryNativeBufferOHOS.html) | `VkResult` | `pBuffer` |

## WSI Surface, Display, Present, Swapchain, And Image Acquire

WSI commands depend on native windows, displays, presentation engines, and
swapchain timing. Many are synchronous queries or present/acquire operations
whose results affect frame pacing and image ownership.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetPhysicalDeviceDisplayPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDisplayPropertiesKHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetPhysicalDeviceDisplayPlanePropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDisplayPlanePropertiesKHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetDisplayPlaneSupportedDisplaysKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDisplayPlaneSupportedDisplaysKHR.html) | `VkResult` | `pDisplayCount`, `pDisplays` |
| [`vkGetDisplayModePropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDisplayModePropertiesKHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetDisplayPlaneCapabilitiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDisplayPlaneCapabilitiesKHR.html) | `VkResult` | `pCapabilities` |
| [`vkGetPhysicalDeviceSurfaceSupportKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceSupportKHR.html) | `VkResult` | `pSupported` |
| [`vkGetPhysicalDeviceSurfaceCapabilitiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceCapabilitiesKHR.html) | `VkResult` | `pSurfaceCapabilities` |
| [`vkGetPhysicalDeviceSurfaceFormatsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceFormatsKHR.html) | `VkResult` | `pSurfaceFormatCount`, `pSurfaceFormats` |
| [`vkGetPhysicalDeviceSurfacePresentModesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfacePresentModesKHR.html) | `VkResult` | `pPresentModeCount`, `pPresentModes` |
| [`vkGetSwapchainImagesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainImagesKHR.html) | `VkResult` | `pSwapchainImageCount`, `pSwapchainImages` |
| [`vkAcquireNextImageKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireNextImageKHR.html) | `VkResult` | `pImageIndex` |
| [`vkQueuePresentKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueuePresentKHR.html) | `VkResult` | - |
| [`vkGetPhysicalDeviceWaylandPresentationSupportKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceWaylandPresentationSupportKHR.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceUbmPresentationSupportSEC`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceUbmPresentationSupportSEC.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceWin32PresentationSupportKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceWin32PresentationSupportKHR.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceXlibPresentationSupportKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceXlibPresentationSupportKHR.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceXcbPresentationSupportKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceXcbPresentationSupportKHR.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceDirectFBPresentationSupportEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDirectFBPresentationSupportEXT.html) | `VkBool32` | - |
| [`vkGetPhysicalDeviceScreenPresentationSupportQNX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceScreenPresentationSupportQNX.html) | `VkBool32` | - |
| [`vkReleaseDisplayEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleaseDisplayEXT.html) | `VkResult` | - |
| [`vkAcquireXlibDisplayEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireXlibDisplayEXT.html) | `VkResult` | - |
| [`vkGetRandROutputDisplayEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRandROutputDisplayEXT.html) | `VkResult` | `pDisplay` |
| [`vkAcquireWinrtDisplayNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireWinrtDisplayNV.html) | `VkResult` | - |
| [`vkGetWinrtDisplayNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetWinrtDisplayNV.html) | `VkResult` | `pDisplay` |
| [`vkDisplayPowerControlEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDisplayPowerControlEXT.html) | `VkResult` | - |
| [`vkGetSwapchainCounterEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainCounterEXT.html) | `VkResult` | `pCounterValue` |
| [`vkGetPhysicalDeviceSurfaceCapabilities2EXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceCapabilities2EXT.html) | `VkResult` | `pSurfaceCapabilities` |
| [`vkGetDeviceGroupPresentCapabilitiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceGroupPresentCapabilitiesKHR.html) | `VkResult` | `pDeviceGroupPresentCapabilities` |
| [`vkGetDeviceGroupSurfacePresentModesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceGroupSurfacePresentModesKHR.html) | `VkResult` | `pModes` |
| [`vkAcquireNextImage2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireNextImage2KHR.html) | `VkResult` | `pImageIndex` |
| [`vkGetPhysicalDevicePresentRectanglesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDevicePresentRectanglesKHR.html) | `VkResult` | `pRectCount`, `pRects` |
| [`vkGetSwapchainStatusKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainStatusKHR.html) | `VkResult` | - |
| [`vkGetRefreshCycleDurationGOOGLE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRefreshCycleDurationGOOGLE.html) | `VkResult` | `pDisplayTimingProperties` |
| [`vkGetPastPresentationTimingGOOGLE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPastPresentationTimingGOOGLE.html) | `VkResult` | `pPresentationTimingCount`, `pPresentationTimings` |
| [`vkGetPhysicalDeviceSurfaceCapabilities2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceCapabilities2KHR.html) | `VkResult` | `pSurfaceCapabilities` |
| [`vkGetPhysicalDeviceSurfaceFormats2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfaceFormats2KHR.html) | `VkResult` | `pSurfaceFormatCount`, `pSurfaceFormats` |
| [`vkGetPhysicalDeviceDisplayProperties2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDisplayProperties2KHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetPhysicalDeviceDisplayPlaneProperties2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDisplayPlaneProperties2KHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetDisplayModeProperties2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDisplayModeProperties2KHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetDisplayPlaneCapabilities2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDisplayPlaneCapabilities2KHR.html) | `VkResult` | `pCapabilities` |
| [`vkGetSwapchainGrallocUsageANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainGrallocUsageANDROID.html) | `VkResult` | `grallocUsage` |
| [`vkGetSwapchainGrallocUsage2ANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainGrallocUsage2ANDROID.html) | `VkResult` | `grallocConsumerUsage`, `grallocProducerUsage` |
| [`vkAcquireImageANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireImageANDROID.html) | `VkResult` | - |
| [`vkQueueSignalReleaseImageANDROID`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSignalReleaseImageANDROID.html) | `VkResult` | `pNativeFenceFd` |
| [`vkGetPhysicalDeviceSurfacePresentModes2EXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSurfacePresentModes2EXT.html) | `VkResult` | `pPresentModeCount`, `pPresentModes` |
| [`vkGetDeviceGroupSurfacePresentModes2EXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceGroupSurfacePresentModes2EXT.html) | `VkResult` | `pModes` |
| [`vkAcquireFullScreenExclusiveModeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireFullScreenExclusiveModeEXT.html) | `VkResult` | - |
| [`vkReleaseFullScreenExclusiveModeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleaseFullScreenExclusiveModeEXT.html) | `VkResult` | - |
| [`vkAcquireDrmDisplayEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireDrmDisplayEXT.html) | `VkResult` | - |
| [`vkGetDrmDisplayEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDrmDisplayEXT.html) | `VkResult` | `display` |
| [`vkWaitForPresent2KHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForPresent2KHR.html) | `VkResult` | - |
| [`vkWaitForPresentKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForPresentKHR.html) | `VkResult` | - |
| [`vkReleaseSwapchainImagesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleaseSwapchainImagesKHR.html) | `VkResult` | - |
| [`vkSetSwapchainPresentTimingQueueSizeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetSwapchainPresentTimingQueueSizeEXT.html) | `VkResult` | - |
| [`vkGetSwapchainTimingPropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainTimingPropertiesEXT.html) | `VkResult` | `pSwapchainTimingProperties`, `pSwapchainTimingPropertiesCounter` |
| [`vkGetSwapchainTimeDomainPropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainTimeDomainPropertiesEXT.html) | `VkResult` | `pSwapchainTimeDomainProperties`, `pTimeDomainsCounter` |
| [`vkGetPastPresentationTimingEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPastPresentationTimingEXT.html) | `VkResult` | `pPastPresentationTimingProperties` |
| [`vkGetSwapchainGrallocUsageOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSwapchainGrallocUsageOHOS.html) | `VkResult` | `grallocUsage` |
| [`vkAcquireImageOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireImageOHOS.html) | `VkResult` | - |
| [`vkQueueSignalReleaseImageOHOS`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSignalReleaseImageOHOS.html) | `VkResult` | `pNativeFenceFd` |

## Enumeration And Two-Call Count/List Queries

These APIs commonly support the Vulkan count-first, fill-second pattern. The
count and data outputs are part of the API contract; forwarding code must
preserve partial-result behavior and `VK_INCOMPLETE` handling where specified.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkEnumeratePhysicalDevices`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumeratePhysicalDevices.html) | `VkResult` | `pPhysicalDeviceCount`, `pPhysicalDevices` |
| [`vkGetPhysicalDeviceQueueFamilyProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyProperties.html) | `void` | `pQueueFamilyPropertyCount`, `pQueueFamilyProperties` |
| [`vkEnumerateInstanceVersion`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumerateInstanceVersion.html) | `VkResult` | `pApiVersion` |
| [`vkEnumerateInstanceLayerProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumerateInstanceLayerProperties.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkEnumerateInstanceExtensionProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumerateInstanceExtensionProperties.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkEnumerateDeviceLayerProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumerateDeviceLayerProperties.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkEnumerateDeviceExtensionProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumerateDeviceExtensionProperties.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetPhysicalDeviceSparseImageFormatProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSparseImageFormatProperties.html) | `void` | `pPropertyCount`, `pProperties` |
| [`vkGetPipelineCacheData`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineCacheData.html) | `VkResult` | `pDataSize`, `pData` |
| [`vkGetPipelineBinaryDataKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineBinaryDataKHR.html) | `VkResult` | `pPipelineBinaryKey`, `pPipelineBinaryDataSize`, `pPipelineBinaryData` |
| [`vkGetPhysicalDeviceQueueFamilyProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyProperties2.html) | `void` | `pQueueFamilyPropertyCount`, `pQueueFamilyProperties` |
| [`vkGetPhysicalDeviceSparseImageFormatProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSparseImageFormatProperties2.html) | `void` | `pPropertyCount`, `pProperties` |
| [`vkEnumeratePhysicalDeviceGroups`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumeratePhysicalDeviceGroups.html) | `VkResult` | `pPhysicalDeviceGroupCount`, `pPhysicalDeviceGroupProperties` |
| [`vkGetValidationCacheDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetValidationCacheDataEXT.html) | `VkResult` | `pDataSize`, `pData` |
| [`vkGetQueueCheckpointDataNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueueCheckpointDataNV.html) | `void` | `pCheckpointDataCount`, `pCheckpointData` |
| [`vkGetPhysicalDeviceCooperativeMatrixPropertiesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceCooperativeMatrixPropertiesNV.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR.html) | `VkResult` | `pCounterCount`, `pCounters`, `pCounterDescriptions` |
| [`vkGetPipelineExecutablePropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineExecutablePropertiesKHR.html) | `VkResult` | `pExecutableCount`, `pProperties` |
| [`vkGetFaultData`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFaultData.html) | `VkResult` | `pUnrecordedFaults`, `pFaultCount`, `pFaults` |
| [`vkGetPhysicalDeviceToolProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceToolProperties.html) | `VkResult` | `pToolCount`, `pToolProperties` |
| [`vkGetPhysicalDeviceFragmentShadingRatesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFragmentShadingRatesKHR.html) | `VkResult` | `pFragmentShadingRateCount`, `pFragmentShadingRates` |
| [`vkGetQueueCheckpointData2NV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueueCheckpointData2NV.html) | `void` | `pCheckpointDataCount`, `pCheckpointData` |
| [`vkGetPhysicalDeviceVideoFormatPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceVideoFormatPropertiesKHR.html) | `VkResult` | `pVideoFormatPropertyCount`, `pVideoFormatProperties` |
| [`vkGetFramebufferTilePropertiesQCOM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFramebufferTilePropertiesQCOM.html) | `VkResult` | `pPropertiesCount`, `pProperties` |
| [`vkGetPhysicalDeviceOpticalFlowImageFormatsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceOpticalFlowImageFormatsNV.html) | `VkResult` | `pFormatCount`, `pImageFormatProperties` |
| [`vkGetShaderBinaryDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetShaderBinaryDataEXT.html) | `VkResult` | `pDataSize`, `pData` |
| [`vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkGetPhysicalDeviceCooperativeVectorPropertiesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceCooperativeVectorPropertiesNV.html) | `VkResult` | `pPropertyCount`, `pProperties` |
| [`vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM.html) | `VkResult` | `pDescriptionCount`, `pDescriptions` |
| [`vkGetShaderInstrumentationValuesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetShaderInstrumentationValuesARM.html) | `VkResult` | `pMetricBlockCount`, `pMetricValues` |
| [`vkGetDataGraphPipelineSessionBindPointRequirementsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDataGraphPipelineSessionBindPointRequirementsARM.html) | `VkResult` | `pBindPointRequirementCount`, `pBindPointRequirements` |
| [`vkGetDataGraphPipelineAvailablePropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDataGraphPipelineAvailablePropertiesARM.html) | `VkResult` | `pPropertiesCount`, `pProperties` |
| [`vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM.html) | `VkResult` | `pQueueFamilyDataGraphPropertyCount`, `pQueueFamilyDataGraphProperties` |
| [`vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM.html) | `VkResult` | `pCounterCount`, `pCounters`, `pCounterDescriptions` |
| [`vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM.html) | `VkResult` | `pFormatCount`, `pImageFormatProperties` |

## Data Readback, Binary/Blob Extraction, And Result Buffers

These APIs return data produced by the driver or replayed workload. They need
owned output storage on the forwarding boundary and should avoid per-call heap
work where result sizes are small or predictable.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetQueryPoolResults`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetQueryPoolResults.html) | `VkResult` | `pData` |
| [`vkMergePipelineCaches`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMergePipelineCaches.html) | `VkResult` | - |
| [`vkReleaseCapturedPipelineDataKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleaseCapturedPipelineDataKHR.html) | `VkResult` | - |
| [`vkMergeValidationCachesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkMergeValidationCachesEXT.html) | `VkResult` | - |
| [`vkGetShaderInfoAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetShaderInfoAMD.html) | `VkResult` | `pInfoSize`, `pInfo` |
| [`vkGetRayTracingShaderGroupHandlesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRayTracingShaderGroupHandlesKHR.html) | `VkResult` | `pData` |
| [`vkGetRayTracingCaptureReplayShaderGroupHandlesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRayTracingCaptureReplayShaderGroupHandlesKHR.html) | `VkResult` | `pData` |
| [`vkGetBufferOpaqueCaptureAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferOpaqueCaptureAddress.html) | `uint64_t` | - |
| [`vkGetDeviceMemoryOpaqueCaptureAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceMemoryOpaqueCaptureAddress.html) | `uint64_t` | - |
| [`vkGetPipelineExecutableStatisticsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineExecutableStatisticsKHR.html) | `VkResult` | `pStatisticCount`, `pStatistics` |
| [`vkGetPipelineExecutableInternalRepresentationsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineExecutableInternalRepresentationsKHR.html) | `VkResult` | `pInternalRepresentationCount`, `pInternalRepresentations` |
| [`vkSetPrivateData`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetPrivateData.html) | `VkResult` | - |
| [`vkGetPrivateData`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPrivateData.html) | `void` | `pData` |
| [`vkGetDescriptorEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorEXT.html) | `void` | `pDescriptor` |
| [`vkGetBufferOpaqueCaptureDescriptorDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferOpaqueCaptureDescriptorDataEXT.html) | `VkResult` | `pData` |
| [`vkGetImageOpaqueCaptureDescriptorDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageOpaqueCaptureDescriptorDataEXT.html) | `VkResult` | `pData` |
| [`vkGetImageViewOpaqueCaptureDescriptorDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewOpaqueCaptureDescriptorDataEXT.html) | `VkResult` | `pData` |
| [`vkGetSamplerOpaqueCaptureDescriptorDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSamplerOpaqueCaptureDescriptorDataEXT.html) | `VkResult` | `pData` |
| [`vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT.html) | `VkResult` | `pData` |
| [`vkGetCudaModuleCacheNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetCudaModuleCacheNV.html) | `VkResult` | `pCacheSize`, `pCacheData` |
| [`vkGetDescriptorSetLayoutHostMappingInfoVALVE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutHostMappingInfoVALVE.html) | `void` | `pHostMapping` |
| [`vkGetShaderModuleIdentifierEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetShaderModuleIdentifierEXT.html) | `void` | `pIdentifier` |
| [`vkGetShaderModuleCreateInfoIdentifierEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetShaderModuleCreateInfoIdentifierEXT.html) | `void` | `pIdentifier` |
| [`vkGetDeviceFaultInfoEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceFaultInfoEXT.html) | `VkResult` | `pFaultCounts`, `pFaultInfo` |
| [`vkGetDeviceFaultReportsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceFaultReportsKHR.html) | `VkResult` | `pFaultCounts`, `pFaultInfo` |
| [`vkGetDeviceFaultDebugInfoKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceFaultDebugInfoKHR.html) | `VkResult` | `pDebugInfo` |
| [`vkGetGpaDeviceClockInfoAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGpaDeviceClockInfoAMD.html) | `VkResult` | `pInfo` |
| [`vkGetGpaSessionResultsAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGpaSessionResultsAMD.html) | `VkResult` | `pSizeInBytes`, `pData` |
| [`vkGetLatencyTimingsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetLatencyTimingsNV.html) | `void` | `pLatencyMarkerInfo` |
| [`vkGetExternalComputeQueueDataNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetExternalComputeQueueDataNV.html) | `void` | `params`, `pData` |
| [`vkGetTensorOpaqueCaptureDescriptorDataARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetTensorOpaqueCaptureDescriptorDataARM.html) | `VkResult` | `pData` |
| [`vkGetTensorViewOpaqueCaptureDescriptorDataARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetTensorViewOpaqueCaptureDescriptorDataARM.html) | `VkResult` | `pData` |
| [`vkBindDataGraphPipelineSessionMemoryARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindDataGraphPipelineSessionMemoryARM.html) | `VkResult` | - |
| [`vkGetDataGraphPipelinePropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDataGraphPipelinePropertiesARM.html) | `VkResult` | `pProperties` |
| [`vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM.html) | `void` | `pQueueFamilyDataGraphProcessingEngineProperties` |
| [`vkGetImageOpaqueCaptureDataEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageOpaqueCaptureDataEXT.html) | `VkResult` | `pDatas` |
| [`vkGetTensorOpaqueCaptureDataARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetTensorOpaqueCaptureDataARM.html) | `VkResult` | `pDatas` |
| [`vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM.html) | `VkResult` | `pProperties` |

## Device/Opaque Address And Small Scalar Handle Queries

These APIs return scalar values that are often meaningful only in the device or
process context that produced them. Replay code must not assume source addresses
or opaque scalar handles are valid receiver-side values.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetImageViewHandleNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewHandleNVX.html) | `uint32_t` | - |
| [`vkGetImageViewHandle64NVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewHandle64NVX.html) | `uint64_t` | - |
| [`vkGetImageViewAddressNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageViewAddressNVX.html) | `VkResult` | `pProperties` |
| [`vkGetDeviceCombinedImageSamplerIndexNVX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceCombinedImageSamplerIndexNVX.html) | `uint64_t` | - |
| [`vkGetBufferDeviceAddress`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetBufferDeviceAddress.html) | `VkDeviceAddress` | - |
| [`vkGetAccelerationStructureDeviceAddressKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureDeviceAddressKHR.html) | `VkDeviceAddress` | - |
| [`vkGetPipelineIndirectDeviceAddressNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineIndirectDeviceAddressNV.html) | `VkDeviceAddress` | - |

## Synchronization, Queue Submission, Waits, Status, And Command Lifecycle Returns

These APIs may have no output parameters, but their return value is observable
and often depends on ordering, externally synchronized state, or GPU progress.
They are not equivalent to void command recording calls.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkQueueSubmit`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSubmit.html) | `VkResult` | - |
| [`vkQueueWaitIdle`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueWaitIdle.html) | `VkResult` | - |
| [`vkDeviceWaitIdle`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDeviceWaitIdle.html) | `VkResult` | - |
| [`vkQueueBindSparse`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueBindSparse.html) | `VkResult` | - |
| [`vkResetFences`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetFences.html) | `VkResult` | - |
| [`vkGetFenceStatus`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetFenceStatus.html) | `VkResult` | - |
| [`vkWaitForFences`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitForFences.html) | `VkResult` | - |
| [`vkGetEventStatus`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetEventStatus.html) | `VkResult` | - |
| [`vkSetEvent`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetEvent.html) | `VkResult` | - |
| [`vkResetEvent`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetEvent.html) | `VkResult` | - |
| [`vkResetDescriptorPool`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetDescriptorPool.html) | `VkResult` | - |
| [`vkFreeDescriptorSets`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkFreeDescriptorSets.html) | `VkResult` | - |
| [`vkResetCommandPool`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetCommandPool.html) | `VkResult` | - |
| [`vkBeginCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBeginCommandBuffer.html) | `VkResult` | - |
| [`vkEndCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkEndCommandBuffer.html) | `VkResult` | - |
| [`vkResetCommandBuffer`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetCommandBuffer.html) | `VkResult` | - |
| [`vkWaitSemaphores`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWaitSemaphores.html) | `VkResult` | - |
| [`vkSignalSemaphore`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSignalSemaphore.html) | `VkResult` | - |
| [`vkCompileDeferredNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCompileDeferredNV.html) | `VkResult` | - |
| [`vkBindAccelerationStructureMemoryNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindAccelerationStructureMemoryNV.html) | `VkResult` | - |
| [`vkCopyAccelerationStructureKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyAccelerationStructureKHR.html) | `VkResult` | - |
| [`vkCopyAccelerationStructureToMemoryKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyAccelerationStructureToMemoryKHR.html) | `VkResult` | - |
| [`vkCopyMemoryToAccelerationStructureKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyMemoryToAccelerationStructureKHR.html) | `VkResult` | - |
| [`vkAcquireProfilingLockKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkAcquireProfilingLockKHR.html) | `VkResult` | - |
| [`vkCmdSetPerformanceMarkerINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceMarkerINTEL.html) | `VkResult` | - |
| [`vkCmdSetPerformanceStreamMarkerINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceStreamMarkerINTEL.html) | `VkResult` | - |
| [`vkCmdSetPerformanceOverrideINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdSetPerformanceOverrideINTEL.html) | `VkResult` | - |
| [`vkReleasePerformanceConfigurationINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkReleasePerformanceConfigurationINTEL.html) | `VkResult` | - |
| [`vkBuildAccelerationStructuresKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBuildAccelerationStructuresKHR.html) | `VkResult` | - |
| [`vkGetDeferredOperationMaxConcurrencyKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeferredOperationMaxConcurrencyKHR.html) | `uint32_t` | - |
| [`vkGetDeferredOperationResultKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeferredOperationResultKHR.html) | `VkResult` | - |
| [`vkDeferredOperationJoinKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDeferredOperationJoinKHR.html) | `VkResult` | - |
| [`vkQueueSubmit2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSubmit2.html) | `VkResult` | - |
| [`vkCopyMemoryToImage`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyMemoryToImage.html) | `VkResult` | - |
| [`vkCopyImageToMemory`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyImageToMemory.html) | `VkResult` | - |
| [`vkCopyImageToImage`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyImageToImage.html) | `VkResult` | - |
| [`vkTransitionImageLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkTransitionImageLayout.html) | `VkResult` | - |
| [`vkBindVideoSessionMemoryKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindVideoSessionMemoryKHR.html) | `VkResult` | - |
| [`vkBuildMicromapsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBuildMicromapsEXT.html) | `VkResult` | - |
| [`vkCopyMicromapEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyMicromapEXT.html) | `VkResult` | - |
| [`vkCopyMicromapToMemoryEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyMicromapToMemoryEXT.html) | `VkResult` | - |
| [`vkCopyMemoryToMicromapEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCopyMemoryToMicromapEXT.html) | `VkResult` | - |
| [`vkBindOpticalFlowSessionImageNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindOpticalFlowSessionImageNV.html) | `VkResult` | - |
| [`vkCmdBeginGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdBeginGpaSessionAMD.html) | `VkResult` | - |
| [`vkCmdEndGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdEndGpaSessionAMD.html) | `VkResult` | - |
| [`vkGetGpaSessionStatusAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetGpaSessionStatusAMD.html) | `VkResult` | - |
| [`vkResetGpaSessionAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkResetGpaSessionAMD.html) | `VkResult` | - |
| [`vkSetLatencySleepModeNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetLatencySleepModeNV.html) | `VkResult` | - |
| [`vkLatencySleepNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkLatencySleepNV.html) | `VkResult` | - |
| [`vkBindTensorMemoryARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkBindTensorMemoryARM.html) | `VkResult` | - |
| [`vkQueueSetPerfHintQCOM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSetPerfHintQCOM.html) | `VkResult` | - |

## Property, Feature, Capability, Support, Compatibility, Size, And Layout Queries

These queries are the best cache candidates, but only when the cache key fully
captures the queried object, input parameters, and `pNext`-defined result shape.
Incorrect cached outputs can change application feature selection and resource
creation behavior.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetPhysicalDeviceProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceProperties.html) | `void` | `pProperties` |
| [`vkGetPhysicalDeviceFeatures`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFeatures.html) | `void` | `pFeatures` |
| [`vkGetPhysicalDeviceFormatProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFormatProperties.html) | `void` | `pFormatProperties` |
| [`vkGetPhysicalDeviceImageFormatProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceImageFormatProperties.html) | `VkResult` | `pImageFormatProperties` |
| [`vkGetImageSubresourceLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSubresourceLayout.html) | `void` | `pLayout` |
| [`vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI.html) | `VkResult` | `pMaxWorkgroupSize` |
| [`vkGetRenderAreaGranularity`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRenderAreaGranularity.html) | `void` | `pGranularity` |
| [`vkGetRenderingAreaGranularity`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRenderingAreaGranularity.html) | `void` | `pGranularity` |
| [`vkGetPhysicalDeviceExternalImageFormatPropertiesNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalImageFormatPropertiesNV.html) | `VkResult` | `pExternalImageFormatProperties` |
| [`vkGetPhysicalDeviceFeatures2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFeatures2.html) | `void` | `pFeatures` |
| [`vkGetPhysicalDeviceProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceProperties2.html) | `void` | `pProperties` |
| [`vkGetPhysicalDeviceFormatProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceFormatProperties2.html) | `void` | `pFormatProperties` |
| [`vkGetPhysicalDeviceImageFormatProperties2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceImageFormatProperties2.html) | `VkResult` | `pImageFormatProperties` |
| [`vkGetPhysicalDeviceExternalBufferProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalBufferProperties.html) | `void` | `pExternalBufferProperties` |
| [`vkGetPhysicalDeviceExternalSemaphoreProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalSemaphoreProperties.html) | `void` | `pExternalSemaphoreProperties` |
| [`vkGetPhysicalDeviceExternalFenceProperties`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalFenceProperties.html) | `void` | `pExternalFenceProperties` |
| [`vkGetDeviceGroupPeerMemoryFeatures`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceGroupPeerMemoryFeatures.html) | `void` | `pPeerMemoryFeatures` |
| [`vkGetPhysicalDeviceMultisamplePropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceMultisamplePropertiesEXT.html) | `void` | `pMultisampleProperties` |
| [`vkGetDescriptorSetLayoutSupport`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutSupport.html) | `void` | `pSupport` |
| [`vkGetPhysicalDeviceCalibrateableTimeDomainsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceCalibrateableTimeDomainsKHR.html) | `VkResult` | `pTimeDomainCount`, `pTimeDomains` |
| [`vkGetMemoryHostPointerPropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetMemoryHostPointerPropertiesEXT.html) | `VkResult` | `pMemoryHostPointerProperties` |
| [`vkGetSemaphoreCounterValue`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetSemaphoreCounterValue.html) | `VkResult` | `pValue` |
| [`vkWriteAccelerationStructuresPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteAccelerationStructuresPropertiesKHR.html) | `VkResult` | `pData` |
| [`vkGetDeviceAccelerationStructureCompatibilityKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceAccelerationStructureCompatibilityKHR.html) | `void` | `pCompatibility` |
| [`vkGetRayTracingShaderGroupStackSizeKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetRayTracingShaderGroupStackSizeKHR.html) | `VkDeviceSize` | - |
| [`vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR.html) | `void` | `pNumPasses` |
| [`vkGetImageDrmFormatModifierPropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageDrmFormatModifierPropertiesEXT.html) | `VkResult` | `pProperties` |
| [`vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV.html) | `VkResult` | `pCombinationCount`, `pCombinations` |
| [`vkGetPerformanceParameterINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPerformanceParameterINTEL.html) | `VkResult` | `pValue` |
| [`vkGetPhysicalDeviceRefreshableObjectTypesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceRefreshableObjectTypesKHR.html) | `VkResult` | `pRefreshableObjectTypeCount`, `pRefreshableObjectTypes` |
| [`vkGetPhysicalDeviceVideoCapabilitiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceVideoCapabilitiesKHR.html) | `VkResult` | `pCapabilities` |
| [`vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR.html) | `VkResult` | `pQualityLevelProperties` |
| [`vkUpdateVideoSessionParametersKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkUpdateVideoSessionParametersKHR.html) | `VkResult` | - |
| [`vkGetEncodedVideoSessionParametersKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetEncodedVideoSessionParametersKHR.html) | `VkResult` | `pFeedbackInfo`, `pDataSize`, `pData` |
| [`vkGetDescriptorSetLayoutSizeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutSizeEXT.html) | `void` | `pLayoutSizeInBytes` |
| [`vkGetDescriptorSetLayoutBindingOffsetEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetLayoutBindingOffsetEXT.html) | `void` | `pOffset` |
| [`vkWriteMicromapsPropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteMicromapsPropertiesEXT.html) | `VkResult` | `pData` |
| [`vkGetDeviceMicromapCompatibilityEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceMicromapCompatibilityEXT.html) | `void` | `pCompatibility` |
| [`vkGetImageSubresourceLayout2`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetImageSubresourceLayout2.html) | `void` | `pLayout` |
| [`vkGetPipelinePropertiesEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelinePropertiesEXT.html) | `VkResult` | `pPipelineProperties` |
| [`vkGetDynamicRenderingTilePropertiesQCOM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDynamicRenderingTilePropertiesQCOM.html) | `VkResult` | `pProperties` |
| [`vkGetDeviceImageSubresourceLayout`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDeviceImageSubresourceLayout.html) | `void` | `pLayout` |
| [`vkGetScreenBufferPropertiesQNX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetScreenBufferPropertiesQNX.html) | `VkResult` | `pProperties` |
| [`vkGetExecutionGraphPipelineScratchSizeAMDX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetExecutionGraphPipelineScratchSizeAMDX.html) | `VkResult` | `pSizeInfo` |
| [`vkGetPhysicalDeviceExternalTensorPropertiesARM`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceExternalTensorPropertiesARM.html) | `void` | `pExternalTensorProperties` |
| [`vkGetPhysicalDeviceDescriptorSizeEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPhysicalDeviceDescriptorSizeEXT.html) | `VkDeviceSize` | - |

## Descriptor Write/Query Helper Commands

These commands interact with descriptor data outside the ordinary descriptor set
allocation path. Returned host mappings or descriptor bytes must not be treated
as stable source-process pointers unless a command-specific policy says so.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetDescriptorSetHostMappingVALVE`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetDescriptorSetHostMappingVALVE.html) | `void` | `ppData` |
| [`vkWriteSamplerDescriptorsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteSamplerDescriptorsEXT.html) | `VkResult` | - |
| [`vkWriteResourceDescriptorsEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkWriteResourceDescriptorsEXT.html) | `VkResult` | - |

## Pipeline, Shader, Ray Tracing, Acceleration-Structure, And Micromap Operations

These are specialized result-producing commands that did not fit the broader
create, query, or data-readback categories cleanly. They need command-specific
replay review before being implemented generically.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkGetPipelineKeyKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetPipelineKeyKHR.html) | `VkResult` | `pPipelineKey` |
| [`vkGetAccelerationStructureHandleNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetAccelerationStructureHandleNV.html) | `VkResult` | `pData` |
| [`vkGetExecutionGraphPipelineNodeIndexAMDX`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetExecutionGraphPipelineNodeIndexAMDX.html) | `VkResult` | `pNodeIndex` |

## Video, Tensor, Data-Graph, Optical-Flow, And Vendor Compute Features

These vendor or domain-specific commands should be treated as explicit policy
work. Placeholder behavior here must not be confused with complete forwarding or
complete Vulkan replay.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkSetGpaDeviceClockModeAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetGpaDeviceClockModeAMD.html) | `VkResult` | `pInfo` |
| [`vkCmdBeginGpaSampleAMD`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkCmdBeginGpaSampleAMD.html) | `VkResult` | `pSampleID` |
| [`vkConvertCooperativeVectorMatrixNV`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkConvertCooperativeVectorMatrixNV.html) | `VkResult` | - |

## Debug, Validation, Fault, Private-Data, Profiling, And Latency Control

These APIs expose diagnostics, instrumentation, timing, or application metadata.
They may be low priority for visual replay, but their return values are still
caller-visible and should have deliberate fallback behavior.

| Command | Return type | Output parameters |
| --- | --- | --- |
| [`vkDebugMarkerSetObjectNameEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDebugMarkerSetObjectNameEXT.html) | `VkResult` | - |
| [`vkDebugMarkerSetObjectTagEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkDebugMarkerSetObjectTagEXT.html) | `VkResult` | - |
| [`vkGetCalibratedTimestampsKHR`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkGetCalibratedTimestampsKHR.html) | `VkResult` | `pTimestamps`, `pMaxDeviation` |
| [`vkSetDebugUtilsObjectNameEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetDebugUtilsObjectNameEXT.html) | `VkResult` | - |
| [`vkSetDebugUtilsObjectTagEXT`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkSetDebugUtilsObjectTagEXT.html) | `VkResult` | - |
| [`vkInitializePerformanceApiINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkInitializePerformanceApiINTEL.html) | `VkResult` | - |
| [`vkQueueSetPerformanceConfigurationINTEL`](https://registry.khronos.org/vulkan/specs/latest/man/html/vkQueueSetPerformanceConfigurationINTEL.html) | `VkResult` | - |
