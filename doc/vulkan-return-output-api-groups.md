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
  `vkCreatePipelineCache` is grouped under handle/object creation because its
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
`vkMapMemory` and `vkMapMemory2` return process-local pointers, so forwarding
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
dispatch commands such as `vkCmdDraw` are not listed because they have no return
value and no output parameters, so they are outside this document's inventory.

| Frequency tier | Commands | Implementation concern |
| --- | --- | --- |
| Per command-buffer recording or per draw-marker path | `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkResetCommandBuffer`, `vkCmdSetPerformanceMarkerINTEL`, `vkCmdSetPerformanceStreamMarkerINTEL`, `vkCmdSetPerformanceOverrideINTEL`, `vkCmdBeginGpaSessionAMD`, `vkCmdEndGpaSessionAMD`, `vkCmdBeginGpaSampleAMD` | These sit near command recording. Even when rarely enabled, the generated path should avoid dynamic lookup, allocation, and avoidable branches. |
| Descriptor-buffer and descriptor helper hot path | `vkGetDescriptorEXT`, `vkWriteSamplerDescriptorsEXT`, `vkWriteResourceDescriptorsEXT`, `vkGetDescriptorSetLayoutSizeEXT`, `vkGetDescriptorSetLayoutBindingOffsetEXT`, `vkGetDescriptorSetHostMappingVALVE`, `vkGetDescriptorSetLayoutHostMappingInfoVALVE` | Descriptor data can be produced while building draw state. Returned host pointers or descriptor bytes need command-specific ownership rules. |
| Dynamic upload, mapped-memory, and visibility path | `vkMapMemory`, `vkMapMemory2`, `vkUnmapMemory2`, `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges` | Engines that map or flush per update can make these expensive. Returned mapped pointers are source-process local and cannot be replayed as raw pointer values. |
| Synchronization polling and frame pacing | `vkGetFenceStatus`, `vkWaitForFences`, `vkResetFences`, `vkGetEventStatus`, `vkSetEvent`, `vkResetEvent`, `vkWaitSemaphores`, `vkSignalSemaphore`, `vkGetSemaphoreCounterValue`, `vkQueueSubmit`, `vkQueueSubmit2`, `vkQueueWaitIdle`, `vkDeviceWaitIdle` | Polling and waits expose real progress. Fake success can break ordering; round trips can stall the CPU side of the frame. |
| Query and instrumentation result readback | `vkGetQueryPoolResults`, `vkGetQueueCheckpointDataNV`, `vkGetQueueCheckpointData2NV`, `vkGetPipelineExecutableStatisticsKHR`, `vkGetPipelineExecutableInternalRepresentationsKHR`, `vkGetGpaSessionStatusAMD`, `vkGetGpaSessionResultsAMD` | Result buffers may be read every frame for timestamps, occlusion, profiling, or diagnostics. Avoid heap churn and preserve partial-result semantics. |
| Address, opaque handle, and descriptor-capture scalar queries | `vkGetBufferDeviceAddress`, `vkGetBufferOpaqueCaptureAddress`, `vkGetDeviceMemoryOpaqueCaptureAddress`, `vkGetAccelerationStructureDeviceAddressKHR`, `vkGetPipelineIndirectDeviceAddressNV`, `vkGetImageViewHandleNVX`, `vkGetImageViewHandle64NVX`, `vkGetDeviceCombinedImageSamplerIndexNVX` | These are cheap scalar returns but may be called while building GPU-visible data. Source scalar values may not be valid receiver-side values. |
| Per-frame WSI and presentation | `vkAcquireNextImageKHR`, `vkAcquireNextImage2KHR`, `vkQueuePresentKHR`, `vkGetSwapchainStatusKHR`, `vkWaitForPresentKHR`, `vkWaitForPresent2KHR`, `vkReleaseSwapchainImagesKHR`, `vkGetPastPresentationTimingGOOGLE`, `vkGetPastPresentationTimingEXT`, `vkGetRefreshCycleDurationGOOGLE`, `vkGetSwapchainCounterEXT` | Usually frame-rate rather than draw-rate, but these are latency-sensitive and directly affect image ownership and pacing. |
| Repeated capability or requirement queries seen in some engines | `vkGetBufferMemoryRequirements`, `vkGetBufferMemoryRequirements2`, `vkGetImageMemoryRequirements`, `vkGetImageMemoryRequirements2`, `vkGetImageSubresourceLayout`, `vkGetImageSubresourceLayout2`, `vkGetPhysicalDeviceFormatProperties`, `vkGetPhysicalDeviceFormatProperties2`, `vkGetPhysicalDeviceImageFormatProperties`, `vkGetPhysicalDeviceImageFormatProperties2` | These should be good cache candidates when the cache key includes all relevant inputs and `pNext` result shape. |

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
| `vkGetDeviceProcAddr` | `PFN_vkVoidFunction` | - |
| `vkGetInstanceProcAddr` | `PFN_vkVoidFunction` | - |

## Handle/Object Creation, Allocation, Acquisition, And Registration

These APIs create or acquire caller-visible handles or handle-like identities.
The source process receives a value immediately, while replay uses a separate
receiver-side handle. The implementation must update the source-to-receiver
handle map only when the call succeeds according to that command's return rules.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkCreateInstance` | `VkResult` | `pInstance` |
| `vkCreateDevice` | `VkResult` | `pDevice` |
| `vkGetDeviceQueue` | `void` | `pQueue` |
| `vkAllocateMemory` | `VkResult` | `pMemory` |
| `vkCreateFence` | `VkResult` | `pFence` |
| `vkCreateSemaphore` | `VkResult` | `pSemaphore` |
| `vkCreateEvent` | `VkResult` | `pEvent` |
| `vkCreateQueryPool` | `VkResult` | `pQueryPool` |
| `vkCreateBuffer` | `VkResult` | `pBuffer` |
| `vkCreateBufferView` | `VkResult` | `pView` |
| `vkCreateImage` | `VkResult` | `pImage` |
| `vkCreateImageView` | `VkResult` | `pView` |
| `vkCreateShaderModule` | `VkResult` | `pShaderModule` |
| `vkCreatePipelineCache` | `VkResult` | `pPipelineCache` |
| `vkCreatePipelineBinariesKHR` | `VkResult` | `pBinaries` |
| `vkCreateGraphicsPipelines` | `VkResult` | `pPipelines` |
| `vkCreateComputePipelines` | `VkResult` | `pPipelines` |
| `vkCreatePipelineLayout` | `VkResult` | `pPipelineLayout` |
| `vkCreateSampler` | `VkResult` | `pSampler` |
| `vkCreateDescriptorSetLayout` | `VkResult` | `pSetLayout` |
| `vkCreateDescriptorPool` | `VkResult` | `pDescriptorPool` |
| `vkAllocateDescriptorSets` | `VkResult` | `pDescriptorSets` |
| `vkCreateFramebuffer` | `VkResult` | `pFramebuffer` |
| `vkCreateRenderPass` | `VkResult` | `pRenderPass` |
| `vkCreateCommandPool` | `VkResult` | `pCommandPool` |
| `vkAllocateCommandBuffers` | `VkResult` | `pCommandBuffers` |
| `vkCreateAndroidSurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateSurfaceOHOS` | `VkResult` | `pSurface` |
| `vkCreateDisplayModeKHR` | `VkResult` | `pMode` |
| `vkCreateDisplayPlaneSurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateSharedSwapchainsKHR` | `VkResult` | `pSwapchains` |
| `vkCreateSwapchainKHR` | `VkResult` | `pSwapchain` |
| `vkCreateViSurfaceNN` | `VkResult` | `pSurface` |
| `vkCreateWaylandSurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateUbmSurfaceSEC` | `VkResult` | `pSurface` |
| `vkCreateWin32SurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateXlibSurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateXcbSurfaceKHR` | `VkResult` | `pSurface` |
| `vkCreateDirectFBSurfaceEXT` | `VkResult` | `pSurface` |
| `vkCreateImagePipeSurfaceFUCHSIA` | `VkResult` | `pSurface` |
| `vkCreateStreamDescriptorSurfaceGGP` | `VkResult` | `pSurface` |
| `vkCreateScreenSurfaceQNX` | `VkResult` | `pSurface` |
| `vkCreateDebugReportCallbackEXT` | `VkResult` | `pCallback` |
| `vkCreateIndirectCommandsLayoutNV` | `VkResult` | `pIndirectCommandsLayout` |
| `vkCreateIndirectCommandsLayoutEXT` | `VkResult` | `pIndirectCommandsLayout` |
| `vkCreateIndirectExecutionSetEXT` | `VkResult` | `pIndirectExecutionSet` |
| `vkCreateSemaphoreSciSyncPoolNV` | `VkResult` | `pSemaphorePool` |
| `vkRegisterDeviceEventEXT` | `VkResult` | `pFence` |
| `vkRegisterDisplayEventEXT` | `VkResult` | `pFence` |
| `vkCreateDescriptorUpdateTemplate` | `VkResult` | `pDescriptorUpdateTemplate` |
| `vkCreateIOSSurfaceMVK` | `VkResult` | `pSurface` |
| `vkCreateMacOSSurfaceMVK` | `VkResult` | `pSurface` |
| `vkCreateMetalSurfaceEXT` | `VkResult` | `pSurface` |
| `vkCreateSamplerYcbcrConversion` | `VkResult` | `pYcbcrConversion` |
| `vkGetDeviceQueue2` | `void` | `pQueue` |
| `vkCreateValidationCacheEXT` | `VkResult` | `pValidationCache` |
| `vkCreateDebugUtilsMessengerEXT` | `VkResult` | `pMessenger` |
| `vkCreateRenderPass2` | `VkResult` | `pRenderPass` |
| `vkCreateAccelerationStructureNV` | `VkResult` | `pAccelerationStructure` |
| `vkCreateRayTracingPipelinesNV` | `VkResult` | `pPipelines` |
| `vkCreateRayTracingPipelinesKHR` | `VkResult` | `pPipelines` |
| `vkCreateHeadlessSurfaceEXT` | `VkResult` | `pSurface` |
| `vkAcquirePerformanceConfigurationINTEL` | `VkResult` | `pConfiguration` |
| `vkCreateAccelerationStructureKHR` | `VkResult` | `pAccelerationStructure` |
| `vkCreateDeferredOperationKHR` | `VkResult` | `pDeferredOperation` |
| `vkCreatePrivateDataSlot` | `VkResult` | `pPrivateDataSlot` |
| `vkCreateVideoSessionKHR` | `VkResult` | `pVideoSession` |
| `vkCreateVideoSessionParametersKHR` | `VkResult` | `pVideoSessionParameters` |
| `vkCreateCuModuleNVX` | `VkResult` | `pModule` |
| `vkCreateCuFunctionNVX` | `VkResult` | `pFunction` |
| `vkCreateBufferCollectionFUCHSIA` | `VkResult` | `pCollection` |
| `vkCreateCudaModuleNV` | `VkResult` | `pModule` |
| `vkCreateCudaFunctionNV` | `VkResult` | `pFunction` |
| `vkCreateMicromapEXT` | `VkResult` | `pMicromap` |
| `vkCreateOpticalFlowSessionNV` | `VkResult` | `pSession` |
| `vkCreateShadersEXT` | `VkResult` | `pShaders` |
| `vkCreateExecutionGraphPipelinesAMDX` | `VkResult` | `pPipelines` |
| `vkCreateGpaSessionAMD` | `VkResult` | `pGpaSession` |
| `vkCreateExternalComputeQueueNV` | `VkResult` | `pExternalQueue` |
| `vkCreateShaderInstrumentationARM` | `VkResult` | `pInstrumentation` |
| `vkCreateTensorARM` | `VkResult` | `pTensor` |
| `vkCreateTensorViewARM` | `VkResult` | `pView` |
| `vkCreateDataGraphPipelinesARM` | `VkResult` | `pPipelines` |
| `vkCreateDataGraphPipelineSessionARM` | `VkResult` | `pSession` |
| `vkRegisterCustomBorderColorEXT` | `VkResult` | `pIndex` |
| `vkCreateAccelerationStructure2KHR` | `VkResult` | `pAccelerationStructure` |

## Memory Map And Unmap

Mapped pointers are process-local and lifetime-sensitive. These APIs need an
explicit forwarding policy because serializing the returned pointer value itself
does not make memory visible in the receiver process.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkMapMemory` | `VkResult` | `ppData` |
| `vkMapMemory2` | `VkResult` | `ppData` |
| `vkUnmapMemory2` | `VkResult` | - |

## Memory Allocation, Binding, Requirements, And Residency/Commitment

These APIs affect memory ownership, compatibility, and binding state. Requirement
queries may be cacheable, but allocation and binding calls change replay state
and must preserve ordering against resource creation and queue work.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetPhysicalDeviceMemoryProperties` | `void` | `pMemoryProperties` |
| `vkFlushMappedMemoryRanges` | `VkResult` | - |
| `vkInvalidateMappedMemoryRanges` | `VkResult` | - |
| `vkGetDeviceMemoryCommitment` | `void` | `pCommittedMemoryInBytes` |
| `vkGetBufferMemoryRequirements` | `void` | `pMemoryRequirements` |
| `vkBindBufferMemory` | `VkResult` | - |
| `vkGetImageMemoryRequirements` | `void` | `pMemoryRequirements` |
| `vkBindImageMemory` | `VkResult` | - |
| `vkGetImageSparseMemoryRequirements` | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| `vkGetGeneratedCommandsMemoryRequirementsNV` | `void` | `pMemoryRequirements` |
| `vkGetGeneratedCommandsMemoryRequirementsEXT` | `void` | `pMemoryRequirements` |
| `vkGetPhysicalDeviceMemoryProperties2` | `void` | `pMemoryProperties` |
| `vkBindBufferMemory2` | `VkResult` | - |
| `vkBindImageMemory2` | `VkResult` | - |
| `vkGetBufferMemoryRequirements2` | `void` | `pMemoryRequirements` |
| `vkGetImageMemoryRequirements2` | `void` | `pMemoryRequirements` |
| `vkGetImageSparseMemoryRequirements2` | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| `vkGetDeviceBufferMemoryRequirements` | `void` | `pMemoryRequirements` |
| `vkGetDeviceImageMemoryRequirements` | `void` | `pMemoryRequirements` |
| `vkGetDeviceImageSparseMemoryRequirements` | `void` | `pSparseMemoryRequirementCount`, `pSparseMemoryRequirements` |
| `vkGetAccelerationStructureMemoryRequirementsNV` | `void` | `pMemoryRequirements` |
| `vkGetClusterAccelerationStructureBuildSizesNV` | `void` | `pSizeInfo` |
| `vkGetPipelineIndirectMemoryRequirementsNV` | `void` | `pMemoryRequirements` |
| `vkGetAccelerationStructureBuildSizesKHR` | `void` | `pSizeInfo` |
| `vkGetCommandPoolMemoryConsumption` | `void` | `pConsumption` |
| `vkGetVideoSessionMemoryRequirementsKHR` | `VkResult` | `pMemoryRequirementsCount`, `pMemoryRequirements` |
| `vkGetPartitionedAccelerationStructuresBuildSizesNV` | `void` | `pSizeInfo` |
| `vkGetMicromapBuildSizesEXT` | `void` | `pSizeInfo` |
| `vkGetTensorMemoryRequirementsARM` | `void` | `pMemoryRequirements` |
| `vkGetDeviceTensorMemoryRequirementsARM` | `void` | `pMemoryRequirements` |
| `vkGetDataGraphPipelineSessionMemoryRequirementsARM` | `void` | `pMemoryRequirements` |

## External Handle Import/Export And Platform Interop

These commands cross OS and process boundaries. Generic replay is usually not
enough; the implementation must define ownership and transfer behavior for file
descriptors, Win32 handles, Zircon handles, Android buffers, Metal objects, and
similar platform resources.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetMemoryWin32HandleNV` | `VkResult` | `pHandle` |
| `vkGetMemoryWin32HandleKHR` | `VkResult` | `pHandle` |
| `vkGetMemoryWin32HandlePropertiesKHR` | `VkResult` | `pMemoryWin32HandleProperties` |
| `vkGetMemoryFdKHR` | `VkResult` | `pFd` |
| `vkGetMemoryFdPropertiesKHR` | `VkResult` | `pMemoryFdProperties` |
| `vkGetMemoryZirconHandleFUCHSIA` | `VkResult` | `pZirconHandle` |
| `vkGetMemoryZirconHandlePropertiesFUCHSIA` | `VkResult` | `pMemoryZirconHandleProperties` |
| `vkGetMemoryRemoteAddressNV` | `VkResult` | `pAddress` |
| `vkGetMemorySciBufNV` | `VkResult` | `pHandle` |
| `vkGetPhysicalDeviceExternalMemorySciBufPropertiesNV` | `VkResult` | `pMemorySciBufProperties` |
| `vkGetPhysicalDeviceSciBufAttributesNV` | `VkResult` | - |
| `vkGetSemaphoreWin32HandleKHR` | `VkResult` | `pHandle` |
| `vkImportSemaphoreWin32HandleKHR` | `VkResult` | - |
| `vkGetSemaphoreFdKHR` | `VkResult` | `pFd` |
| `vkImportSemaphoreFdKHR` | `VkResult` | - |
| `vkGetSemaphoreZirconHandleFUCHSIA` | `VkResult` | `pZirconHandle` |
| `vkImportSemaphoreZirconHandleFUCHSIA` | `VkResult` | - |
| `vkGetFenceWin32HandleKHR` | `VkResult` | `pHandle` |
| `vkImportFenceWin32HandleKHR` | `VkResult` | - |
| `vkGetFenceFdKHR` | `VkResult` | `pFd` |
| `vkImportFenceFdKHR` | `VkResult` | - |
| `vkGetFenceSciSyncFenceNV` | `VkResult` | `pHandle` |
| `vkGetFenceSciSyncObjNV` | `VkResult` | `pHandle` |
| `vkImportFenceSciSyncFenceNV` | `VkResult` | - |
| `vkImportFenceSciSyncObjNV` | `VkResult` | - |
| `vkGetSemaphoreSciSyncObjNV` | `VkResult` | `pHandle` |
| `vkImportSemaphoreSciSyncObjNV` | `VkResult` | - |
| `vkGetPhysicalDeviceSciSyncAttributesNV` | `VkResult` | - |
| `vkGetAndroidHardwareBufferPropertiesANDROID` | `VkResult` | `pProperties` |
| `vkGetMemoryAndroidHardwareBufferANDROID` | `VkResult` | `pBuffer` |
| `vkSetBufferCollectionBufferConstraintsFUCHSIA` | `VkResult` | - |
| `vkSetBufferCollectionImageConstraintsFUCHSIA` | `VkResult` | - |
| `vkGetBufferCollectionPropertiesFUCHSIA` | `VkResult` | `pProperties` |
| `vkExportMetalObjectsEXT` | `void` | `pMetalObjectsInfo` |
| `vkGetMemoryMetalHandleEXT` | `VkResult` | `pHandle` |
| `vkGetMemoryMetalHandlePropertiesEXT` | `VkResult` | `pMemoryMetalHandleProperties` |
| `vkGetNativeBufferPropertiesOHOS` | `VkResult` | `pProperties` |
| `vkGetMemoryNativeBufferOHOS` | `VkResult` | `pBuffer` |

## WSI Surface, Display, Present, Swapchain, And Image Acquire

WSI commands depend on native windows, displays, presentation engines, and
swapchain timing. Many are synchronous queries or present/acquire operations
whose results affect frame pacing and image ownership.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetPhysicalDeviceDisplayPropertiesKHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetPhysicalDeviceDisplayPlanePropertiesKHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetDisplayPlaneSupportedDisplaysKHR` | `VkResult` | `pDisplayCount`, `pDisplays` |
| `vkGetDisplayModePropertiesKHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetDisplayPlaneCapabilitiesKHR` | `VkResult` | `pCapabilities` |
| `vkGetPhysicalDeviceSurfaceSupportKHR` | `VkResult` | `pSupported` |
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` | `VkResult` | `pSurfaceCapabilities` |
| `vkGetPhysicalDeviceSurfaceFormatsKHR` | `VkResult` | `pSurfaceFormatCount`, `pSurfaceFormats` |
| `vkGetPhysicalDeviceSurfacePresentModesKHR` | `VkResult` | `pPresentModeCount`, `pPresentModes` |
| `vkGetSwapchainImagesKHR` | `VkResult` | `pSwapchainImageCount`, `pSwapchainImages` |
| `vkAcquireNextImageKHR` | `VkResult` | `pImageIndex` |
| `vkQueuePresentKHR` | `VkResult` | - |
| `vkGetPhysicalDeviceWaylandPresentationSupportKHR` | `VkBool32` | - |
| `vkGetPhysicalDeviceUbmPresentationSupportSEC` | `VkBool32` | - |
| `vkGetPhysicalDeviceWin32PresentationSupportKHR` | `VkBool32` | - |
| `vkGetPhysicalDeviceXlibPresentationSupportKHR` | `VkBool32` | - |
| `vkGetPhysicalDeviceXcbPresentationSupportKHR` | `VkBool32` | - |
| `vkGetPhysicalDeviceDirectFBPresentationSupportEXT` | `VkBool32` | - |
| `vkGetPhysicalDeviceScreenPresentationSupportQNX` | `VkBool32` | - |
| `vkReleaseDisplayEXT` | `VkResult` | - |
| `vkAcquireXlibDisplayEXT` | `VkResult` | - |
| `vkGetRandROutputDisplayEXT` | `VkResult` | `pDisplay` |
| `vkAcquireWinrtDisplayNV` | `VkResult` | - |
| `vkGetWinrtDisplayNV` | `VkResult` | `pDisplay` |
| `vkDisplayPowerControlEXT` | `VkResult` | - |
| `vkGetSwapchainCounterEXT` | `VkResult` | `pCounterValue` |
| `vkGetPhysicalDeviceSurfaceCapabilities2EXT` | `VkResult` | `pSurfaceCapabilities` |
| `vkGetDeviceGroupPresentCapabilitiesKHR` | `VkResult` | `pDeviceGroupPresentCapabilities` |
| `vkGetDeviceGroupSurfacePresentModesKHR` | `VkResult` | `pModes` |
| `vkAcquireNextImage2KHR` | `VkResult` | `pImageIndex` |
| `vkGetPhysicalDevicePresentRectanglesKHR` | `VkResult` | `pRectCount`, `pRects` |
| `vkGetSwapchainStatusKHR` | `VkResult` | - |
| `vkGetRefreshCycleDurationGOOGLE` | `VkResult` | `pDisplayTimingProperties` |
| `vkGetPastPresentationTimingGOOGLE` | `VkResult` | `pPresentationTimingCount`, `pPresentationTimings` |
| `vkGetPhysicalDeviceSurfaceCapabilities2KHR` | `VkResult` | `pSurfaceCapabilities` |
| `vkGetPhysicalDeviceSurfaceFormats2KHR` | `VkResult` | `pSurfaceFormatCount`, `pSurfaceFormats` |
| `vkGetPhysicalDeviceDisplayProperties2KHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetPhysicalDeviceDisplayPlaneProperties2KHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetDisplayModeProperties2KHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetDisplayPlaneCapabilities2KHR` | `VkResult` | `pCapabilities` |
| `vkGetSwapchainGrallocUsageANDROID` | `VkResult` | `grallocUsage` |
| `vkGetSwapchainGrallocUsage2ANDROID` | `VkResult` | `grallocConsumerUsage`, `grallocProducerUsage` |
| `vkAcquireImageANDROID` | `VkResult` | - |
| `vkQueueSignalReleaseImageANDROID` | `VkResult` | `pNativeFenceFd` |
| `vkGetPhysicalDeviceSurfacePresentModes2EXT` | `VkResult` | `pPresentModeCount`, `pPresentModes` |
| `vkGetDeviceGroupSurfacePresentModes2EXT` | `VkResult` | `pModes` |
| `vkAcquireFullScreenExclusiveModeEXT` | `VkResult` | - |
| `vkReleaseFullScreenExclusiveModeEXT` | `VkResult` | - |
| `vkAcquireDrmDisplayEXT` | `VkResult` | - |
| `vkGetDrmDisplayEXT` | `VkResult` | `display` |
| `vkWaitForPresent2KHR` | `VkResult` | - |
| `vkWaitForPresentKHR` | `VkResult` | - |
| `vkReleaseSwapchainImagesKHR` | `VkResult` | - |
| `vkSetSwapchainPresentTimingQueueSizeEXT` | `VkResult` | - |
| `vkGetSwapchainTimingPropertiesEXT` | `VkResult` | `pSwapchainTimingProperties`, `pSwapchainTimingPropertiesCounter` |
| `vkGetSwapchainTimeDomainPropertiesEXT` | `VkResult` | `pSwapchainTimeDomainProperties`, `pTimeDomainsCounter` |
| `vkGetPastPresentationTimingEXT` | `VkResult` | `pPastPresentationTimingProperties` |
| `vkGetSwapchainGrallocUsageOHOS` | `VkResult` | `grallocUsage` |
| `vkAcquireImageOHOS` | `VkResult` | - |
| `vkQueueSignalReleaseImageOHOS` | `VkResult` | `pNativeFenceFd` |

## Enumeration And Two-Call Count/List Queries

These APIs commonly support the Vulkan count-first, fill-second pattern. The
count and data outputs are part of the API contract; forwarding code must
preserve partial-result behavior and `VK_INCOMPLETE` handling where specified.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkEnumeratePhysicalDevices` | `VkResult` | `pPhysicalDeviceCount`, `pPhysicalDevices` |
| `vkGetPhysicalDeviceQueueFamilyProperties` | `void` | `pQueueFamilyPropertyCount`, `pQueueFamilyProperties` |
| `vkEnumerateInstanceVersion` | `VkResult` | `pApiVersion` |
| `vkEnumerateInstanceLayerProperties` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkEnumerateInstanceExtensionProperties` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkEnumerateDeviceLayerProperties` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkEnumerateDeviceExtensionProperties` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetPhysicalDeviceSparseImageFormatProperties` | `void` | `pPropertyCount`, `pProperties` |
| `vkGetPipelineCacheData` | `VkResult` | `pDataSize`, `pData` |
| `vkGetPipelineBinaryDataKHR` | `VkResult` | `pPipelineBinaryKey`, `pPipelineBinaryDataSize`, `pPipelineBinaryData` |
| `vkGetPhysicalDeviceQueueFamilyProperties2` | `void` | `pQueueFamilyPropertyCount`, `pQueueFamilyProperties` |
| `vkGetPhysicalDeviceSparseImageFormatProperties2` | `void` | `pPropertyCount`, `pProperties` |
| `vkEnumeratePhysicalDeviceGroups` | `VkResult` | `pPhysicalDeviceGroupCount`, `pPhysicalDeviceGroupProperties` |
| `vkGetValidationCacheDataEXT` | `VkResult` | `pDataSize`, `pData` |
| `vkGetQueueCheckpointDataNV` | `void` | `pCheckpointDataCount`, `pCheckpointData` |
| `vkGetPhysicalDeviceCooperativeMatrixPropertiesNV` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR` | `VkResult` | `pCounterCount`, `pCounters`, `pCounterDescriptions` |
| `vkGetPipelineExecutablePropertiesKHR` | `VkResult` | `pExecutableCount`, `pProperties` |
| `vkGetFaultData` | `VkResult` | `pUnrecordedFaults`, `pFaultCount`, `pFaults` |
| `vkGetPhysicalDeviceToolProperties` | `VkResult` | `pToolCount`, `pToolProperties` |
| `vkGetPhysicalDeviceFragmentShadingRatesKHR` | `VkResult` | `pFragmentShadingRateCount`, `pFragmentShadingRates` |
| `vkGetQueueCheckpointData2NV` | `void` | `pCheckpointDataCount`, `pCheckpointData` |
| `vkGetPhysicalDeviceVideoFormatPropertiesKHR` | `VkResult` | `pVideoFormatPropertyCount`, `pVideoFormatProperties` |
| `vkGetFramebufferTilePropertiesQCOM` | `VkResult` | `pPropertiesCount`, `pProperties` |
| `vkGetPhysicalDeviceOpticalFlowImageFormatsNV` | `VkResult` | `pFormatCount`, `pImageFormatProperties` |
| `vkGetShaderBinaryDataEXT` | `VkResult` | `pDataSize`, `pData` |
| `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkGetPhysicalDeviceCooperativeVectorPropertiesNV` | `VkResult` | `pPropertyCount`, `pProperties` |
| `vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM` | `VkResult` | `pDescriptionCount`, `pDescriptions` |
| `vkGetShaderInstrumentationValuesARM` | `VkResult` | `pMetricBlockCount`, `pMetricValues` |
| `vkGetDataGraphPipelineSessionBindPointRequirementsARM` | `VkResult` | `pBindPointRequirementCount`, `pBindPointRequirements` |
| `vkGetDataGraphPipelineAvailablePropertiesARM` | `VkResult` | `pPropertiesCount`, `pProperties` |
| `vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM` | `VkResult` | `pQueueFamilyDataGraphPropertyCount`, `pQueueFamilyDataGraphProperties` |
| `vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM` | `VkResult` | `pCounterCount`, `pCounters`, `pCounterDescriptions` |
| `vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM` | `VkResult` | `pFormatCount`, `pImageFormatProperties` |

## Data Readback, Binary/Blob Extraction, And Result Buffers

These APIs return data produced by the driver or replayed workload. They need
owned output storage on the forwarding boundary and should avoid per-call heap
work where result sizes are small or predictable.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetQueryPoolResults` | `VkResult` | `pData` |
| `vkMergePipelineCaches` | `VkResult` | - |
| `vkReleaseCapturedPipelineDataKHR` | `VkResult` | - |
| `vkMergeValidationCachesEXT` | `VkResult` | - |
| `vkGetShaderInfoAMD` | `VkResult` | `pInfoSize`, `pInfo` |
| `vkGetRayTracingShaderGroupHandlesKHR` | `VkResult` | `pData` |
| `vkGetRayTracingCaptureReplayShaderGroupHandlesKHR` | `VkResult` | `pData` |
| `vkGetBufferOpaqueCaptureAddress` | `uint64_t` | - |
| `vkGetDeviceMemoryOpaqueCaptureAddress` | `uint64_t` | - |
| `vkGetPipelineExecutableStatisticsKHR` | `VkResult` | `pStatisticCount`, `pStatistics` |
| `vkGetPipelineExecutableInternalRepresentationsKHR` | `VkResult` | `pInternalRepresentationCount`, `pInternalRepresentations` |
| `vkSetPrivateData` | `VkResult` | - |
| `vkGetPrivateData` | `void` | `pData` |
| `vkGetDescriptorEXT` | `void` | `pDescriptor` |
| `vkGetBufferOpaqueCaptureDescriptorDataEXT` | `VkResult` | `pData` |
| `vkGetImageOpaqueCaptureDescriptorDataEXT` | `VkResult` | `pData` |
| `vkGetImageViewOpaqueCaptureDescriptorDataEXT` | `VkResult` | `pData` |
| `vkGetSamplerOpaqueCaptureDescriptorDataEXT` | `VkResult` | `pData` |
| `vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT` | `VkResult` | `pData` |
| `vkGetCudaModuleCacheNV` | `VkResult` | `pCacheSize`, `pCacheData` |
| `vkGetDescriptorSetLayoutHostMappingInfoVALVE` | `void` | `pHostMapping` |
| `vkGetShaderModuleIdentifierEXT` | `void` | `pIdentifier` |
| `vkGetShaderModuleCreateInfoIdentifierEXT` | `void` | `pIdentifier` |
| `vkGetDeviceFaultInfoEXT` | `VkResult` | `pFaultCounts`, `pFaultInfo` |
| `vkGetDeviceFaultReportsKHR` | `VkResult` | `pFaultCounts`, `pFaultInfo` |
| `vkGetDeviceFaultDebugInfoKHR` | `VkResult` | `pDebugInfo` |
| `vkGetGpaDeviceClockInfoAMD` | `VkResult` | `pInfo` |
| `vkGetGpaSessionResultsAMD` | `VkResult` | `pSizeInBytes`, `pData` |
| `vkGetLatencyTimingsNV` | `void` | `pLatencyMarkerInfo` |
| `vkGetExternalComputeQueueDataNV` | `void` | `params`, `pData` |
| `vkGetTensorOpaqueCaptureDescriptorDataARM` | `VkResult` | `pData` |
| `vkGetTensorViewOpaqueCaptureDescriptorDataARM` | `VkResult` | `pData` |
| `vkBindDataGraphPipelineSessionMemoryARM` | `VkResult` | - |
| `vkGetDataGraphPipelinePropertiesARM` | `VkResult` | `pProperties` |
| `vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM` | `void` | `pQueueFamilyDataGraphProcessingEngineProperties` |
| `vkGetImageOpaqueCaptureDataEXT` | `VkResult` | `pDatas` |
| `vkGetTensorOpaqueCaptureDataARM` | `VkResult` | `pDatas` |
| `vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM` | `VkResult` | `pProperties` |

## Device/Opaque Address And Small Scalar Handle Queries

These APIs return scalar values that are often meaningful only in the device or
process context that produced them. Replay code must not assume source addresses
or opaque scalar handles are valid receiver-side values.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetImageViewHandleNVX` | `uint32_t` | - |
| `vkGetImageViewHandle64NVX` | `uint64_t` | - |
| `vkGetImageViewAddressNVX` | `VkResult` | `pProperties` |
| `vkGetDeviceCombinedImageSamplerIndexNVX` | `uint64_t` | - |
| `vkGetBufferDeviceAddress` | `VkDeviceAddress` | - |
| `vkGetAccelerationStructureDeviceAddressKHR` | `VkDeviceAddress` | - |
| `vkGetPipelineIndirectDeviceAddressNV` | `VkDeviceAddress` | - |

## Synchronization, Queue Submission, Waits, Status, And Command Lifecycle Returns

These APIs may have no output parameters, but their return value is observable
and often depends on ordering, externally synchronized state, or GPU progress.
They are not equivalent to void command recording calls.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkQueueSubmit` | `VkResult` | - |
| `vkQueueWaitIdle` | `VkResult` | - |
| `vkDeviceWaitIdle` | `VkResult` | - |
| `vkQueueBindSparse` | `VkResult` | - |
| `vkResetFences` | `VkResult` | - |
| `vkGetFenceStatus` | `VkResult` | - |
| `vkWaitForFences` | `VkResult` | - |
| `vkGetEventStatus` | `VkResult` | - |
| `vkSetEvent` | `VkResult` | - |
| `vkResetEvent` | `VkResult` | - |
| `vkResetDescriptorPool` | `VkResult` | - |
| `vkFreeDescriptorSets` | `VkResult` | - |
| `vkResetCommandPool` | `VkResult` | - |
| `vkBeginCommandBuffer` | `VkResult` | - |
| `vkEndCommandBuffer` | `VkResult` | - |
| `vkResetCommandBuffer` | `VkResult` | - |
| `vkWaitSemaphores` | `VkResult` | - |
| `vkSignalSemaphore` | `VkResult` | - |
| `vkCompileDeferredNV` | `VkResult` | - |
| `vkBindAccelerationStructureMemoryNV` | `VkResult` | - |
| `vkCopyAccelerationStructureKHR` | `VkResult` | - |
| `vkCopyAccelerationStructureToMemoryKHR` | `VkResult` | - |
| `vkCopyMemoryToAccelerationStructureKHR` | `VkResult` | - |
| `vkAcquireProfilingLockKHR` | `VkResult` | - |
| `vkCmdSetPerformanceMarkerINTEL` | `VkResult` | - |
| `vkCmdSetPerformanceStreamMarkerINTEL` | `VkResult` | - |
| `vkCmdSetPerformanceOverrideINTEL` | `VkResult` | - |
| `vkReleasePerformanceConfigurationINTEL` | `VkResult` | - |
| `vkBuildAccelerationStructuresKHR` | `VkResult` | - |
| `vkGetDeferredOperationMaxConcurrencyKHR` | `uint32_t` | - |
| `vkGetDeferredOperationResultKHR` | `VkResult` | - |
| `vkDeferredOperationJoinKHR` | `VkResult` | - |
| `vkQueueSubmit2` | `VkResult` | - |
| `vkCopyMemoryToImage` | `VkResult` | - |
| `vkCopyImageToMemory` | `VkResult` | - |
| `vkCopyImageToImage` | `VkResult` | - |
| `vkTransitionImageLayout` | `VkResult` | - |
| `vkBindVideoSessionMemoryKHR` | `VkResult` | - |
| `vkBuildMicromapsEXT` | `VkResult` | - |
| `vkCopyMicromapEXT` | `VkResult` | - |
| `vkCopyMicromapToMemoryEXT` | `VkResult` | - |
| `vkCopyMemoryToMicromapEXT` | `VkResult` | - |
| `vkBindOpticalFlowSessionImageNV` | `VkResult` | - |
| `vkCmdBeginGpaSessionAMD` | `VkResult` | - |
| `vkCmdEndGpaSessionAMD` | `VkResult` | - |
| `vkGetGpaSessionStatusAMD` | `VkResult` | - |
| `vkResetGpaSessionAMD` | `VkResult` | - |
| `vkSetLatencySleepModeNV` | `VkResult` | - |
| `vkLatencySleepNV` | `VkResult` | - |
| `vkBindTensorMemoryARM` | `VkResult` | - |
| `vkQueueSetPerfHintQCOM` | `VkResult` | - |

## Property, Feature, Capability, Support, Compatibility, Size, And Layout Queries

These queries are the best cache candidates, but only when the cache key fully
captures the queried object, input parameters, and `pNext`-defined result shape.
Incorrect cached outputs can change application feature selection and resource
creation behavior.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetPhysicalDeviceProperties` | `void` | `pProperties` |
| `vkGetPhysicalDeviceFeatures` | `void` | `pFeatures` |
| `vkGetPhysicalDeviceFormatProperties` | `void` | `pFormatProperties` |
| `vkGetPhysicalDeviceImageFormatProperties` | `VkResult` | `pImageFormatProperties` |
| `vkGetImageSubresourceLayout` | `void` | `pLayout` |
| `vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI` | `VkResult` | `pMaxWorkgroupSize` |
| `vkGetRenderAreaGranularity` | `void` | `pGranularity` |
| `vkGetRenderingAreaGranularity` | `void` | `pGranularity` |
| `vkGetPhysicalDeviceExternalImageFormatPropertiesNV` | `VkResult` | `pExternalImageFormatProperties` |
| `vkGetPhysicalDeviceFeatures2` | `void` | `pFeatures` |
| `vkGetPhysicalDeviceProperties2` | `void` | `pProperties` |
| `vkGetPhysicalDeviceFormatProperties2` | `void` | `pFormatProperties` |
| `vkGetPhysicalDeviceImageFormatProperties2` | `VkResult` | `pImageFormatProperties` |
| `vkGetPhysicalDeviceExternalBufferProperties` | `void` | `pExternalBufferProperties` |
| `vkGetPhysicalDeviceExternalSemaphoreProperties` | `void` | `pExternalSemaphoreProperties` |
| `vkGetPhysicalDeviceExternalFenceProperties` | `void` | `pExternalFenceProperties` |
| `vkGetDeviceGroupPeerMemoryFeatures` | `void` | `pPeerMemoryFeatures` |
| `vkGetPhysicalDeviceMultisamplePropertiesEXT` | `void` | `pMultisampleProperties` |
| `vkGetDescriptorSetLayoutSupport` | `void` | `pSupport` |
| `vkGetPhysicalDeviceCalibrateableTimeDomainsKHR` | `VkResult` | `pTimeDomainCount`, `pTimeDomains` |
| `vkGetMemoryHostPointerPropertiesEXT` | `VkResult` | `pMemoryHostPointerProperties` |
| `vkGetSemaphoreCounterValue` | `VkResult` | `pValue` |
| `vkWriteAccelerationStructuresPropertiesKHR` | `VkResult` | `pData` |
| `vkGetDeviceAccelerationStructureCompatibilityKHR` | `void` | `pCompatibility` |
| `vkGetRayTracingShaderGroupStackSizeKHR` | `VkDeviceSize` | - |
| `vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR` | `void` | `pNumPasses` |
| `vkGetImageDrmFormatModifierPropertiesEXT` | `VkResult` | `pProperties` |
| `vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV` | `VkResult` | `pCombinationCount`, `pCombinations` |
| `vkGetPerformanceParameterINTEL` | `VkResult` | `pValue` |
| `vkGetPhysicalDeviceRefreshableObjectTypesKHR` | `VkResult` | `pRefreshableObjectTypeCount`, `pRefreshableObjectTypes` |
| `vkGetPhysicalDeviceVideoCapabilitiesKHR` | `VkResult` | `pCapabilities` |
| `vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR` | `VkResult` | `pQualityLevelProperties` |
| `vkUpdateVideoSessionParametersKHR` | `VkResult` | - |
| `vkGetEncodedVideoSessionParametersKHR` | `VkResult` | `pFeedbackInfo`, `pDataSize`, `pData` |
| `vkGetDescriptorSetLayoutSizeEXT` | `void` | `pLayoutSizeInBytes` |
| `vkGetDescriptorSetLayoutBindingOffsetEXT` | `void` | `pOffset` |
| `vkWriteMicromapsPropertiesEXT` | `VkResult` | `pData` |
| `vkGetDeviceMicromapCompatibilityEXT` | `void` | `pCompatibility` |
| `vkGetImageSubresourceLayout2` | `void` | `pLayout` |
| `vkGetPipelinePropertiesEXT` | `VkResult` | `pPipelineProperties` |
| `vkGetDynamicRenderingTilePropertiesQCOM` | `VkResult` | `pProperties` |
| `vkGetDeviceImageSubresourceLayout` | `void` | `pLayout` |
| `vkGetScreenBufferPropertiesQNX` | `VkResult` | `pProperties` |
| `vkGetExecutionGraphPipelineScratchSizeAMDX` | `VkResult` | `pSizeInfo` |
| `vkGetPhysicalDeviceExternalTensorPropertiesARM` | `void` | `pExternalTensorProperties` |
| `vkGetPhysicalDeviceDescriptorSizeEXT` | `VkDeviceSize` | - |

## Descriptor Write/Query Helper Commands

These commands interact with descriptor data outside the ordinary descriptor set
allocation path. Returned host mappings or descriptor bytes must not be treated
as stable source-process pointers unless a command-specific policy says so.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetDescriptorSetHostMappingVALVE` | `void` | `ppData` |
| `vkWriteSamplerDescriptorsEXT` | `VkResult` | - |
| `vkWriteResourceDescriptorsEXT` | `VkResult` | - |

## Pipeline, Shader, Ray Tracing, Acceleration-Structure, And Micromap Operations

These are specialized result-producing commands that did not fit the broader
create, query, or data-readback categories cleanly. They need command-specific
replay review before being implemented generically.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkGetPipelineKeyKHR` | `VkResult` | `pPipelineKey` |
| `vkGetAccelerationStructureHandleNV` | `VkResult` | `pData` |
| `vkGetExecutionGraphPipelineNodeIndexAMDX` | `VkResult` | `pNodeIndex` |

## Video, Tensor, Data-Graph, Optical-Flow, And Vendor Compute Features

These vendor or domain-specific commands should be treated as explicit policy
work. Placeholder behavior here must not be confused with complete forwarding or
complete Vulkan replay.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkSetGpaDeviceClockModeAMD` | `VkResult` | `pInfo` |
| `vkCmdBeginGpaSampleAMD` | `VkResult` | `pSampleID` |
| `vkConvertCooperativeVectorMatrixNV` | `VkResult` | - |

## Debug, Validation, Fault, Private-Data, Profiling, And Latency Control

These APIs expose diagnostics, instrumentation, timing, or application metadata.
They may be low priority for visual replay, but their return values are still
caller-visible and should have deliberate fallback behavior.

| Command | Return type | Output parameters |
| --- | --- | --- |
| `vkDebugMarkerSetObjectNameEXT` | `VkResult` | - |
| `vkDebugMarkerSetObjectTagEXT` | `VkResult` | - |
| `vkGetCalibratedTimestampsKHR` | `VkResult` | `pTimestamps`, `pMaxDeviation` |
| `vkSetDebugUtilsObjectNameEXT` | `VkResult` | - |
| `vkSetDebugUtilsObjectTagEXT` | `VkResult` | - |
| `vkInitializePerformanceApiINTEL` | `VkResult` | - |
| `vkQueueSetPerformanceConfigurationINTEL` | `VkResult` | - |
