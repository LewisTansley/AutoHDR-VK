#include "layer_common.hpp"
#include "config.hpp"
#include "input.hpp"
#include "runtime_ui.hpp"

#ifndef VK_LAYER_EXPORT
#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace AutoHdrVk {

namespace {

template <typename T>
T getProc(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char *name)
{
    return reinterpret_cast<T>(gipa(instance, name));
}

template <typename T>
T getProc(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char *name)
{
    return reinterpret_cast<T>(gdpa(device, name));
}

bool isHdrColorSpace(VkColorSpaceKHR cs)
{
    return cs == VK_COLOR_SPACE_HDR10_ST2084_EXT || cs == VK_COLOR_SPACE_HDR10_HLG_EXT
        || cs == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT || cs == VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT;
}

OutputEncoding resolveEncoding(VkColorSpaceKHR cs)
{
    OutputEncoding cfg = activeEncoding();
    if (cfg != OutputEncoding::Auto) {
        return cfg;
    }
    if (cs == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
        return OutputEncoding::LinearScRgb;
    }
    if (cs == VK_COLOR_SPACE_HDR10_ST2084_EXT || cs == VK_COLOR_SPACE_HDR10_HLG_EXT) {
        return OutputEncoding::Pq;
    }
    return OutputEncoding::SdrPreview;
}

float encodingToOutputMode(OutputEncoding encoding)
{
    switch (encoding) {
    case OutputEncoding::LinearScRgb:
        return 1.0f;
    case OutputEncoding::SdrPreview:
        return 2.0f;
    default:
        return 0.0f;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(VkInstance instance,
                                                       const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                                                       const VkAllocationCallbacks *pAllocator,
                                                       VkSurfaceKHR *pSurface)
{
    InstanceData *data = getInstanceData(instance);
    if (!data || !data->dispatch.CreateWaylandSurfaceKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = data->dispatch.CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    if (result == VK_SUCCESS && pCreateInfo && pSurface) {
        registerWaylandSurface(reinterpret_cast<void *>(*pSurface), pCreateInfo->display);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    InstanceData *data = getInstanceData(instance);
    if (!data || !data->dispatch.CreateXlibSurfaceKHR) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    const VkResult result = data->dispatch.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    if (result == VK_SUCCESS && pCreateInfo && pSurface) {
        registerX11Window(reinterpret_cast<void *>(*pSurface), static_cast<unsigned long>(pCreateInfo->window));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    InstanceData *data = getInstanceData(instance);
    if (!data || !data->dispatch.CreateXcbSurfaceKHR) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    const VkResult result = data->dispatch.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    if (result == VK_SUCCESS && pCreateInfo && pSurface) {
        // XCB window ids are interchangeable with Xlib Window on the same server.
        registerX11Window(reinterpret_cast<void *>(*pSurface), static_cast<unsigned long>(pCreateInfo->window));
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks *pAllocator)
{
    unregisterWaylandSurface(reinterpret_cast<void *>(surface));
    unregisterX11Window(reinterpret_cast<void *>(surface));
    InstanceData *data = getInstanceData(instance);
    if (data && data->dispatch.DestroySurfaceKHR) {
        data->dispatch.DestroySurfaceKHR(instance, surface, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *layerCreateInfo = nullptr;
    for (auto *node = static_cast<const VkBaseInStructure *>(pCreateInfo->pNext); node;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto *candidate = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<VkBaseInStructure *>(node));
            if (candidate->function == VK_LAYER_LINK_INFO) {
                layerCreateInfo = candidate;
                break;
            }
        }
    }
    if (!layerCreateInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    auto nextCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult result = nextCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }

    reloadConfig();

    auto *data = new InstanceData();
    data->instance = *pInstance;
    data->dispatch.GetInstanceProcAddr = gipa;
    data->dispatch.DestroyInstance = getProc<PFN_vkDestroyInstance>(gipa, *pInstance, "vkDestroyInstance");
    data->dispatch.CreateDevice = getProc<PFN_vkCreateDevice>(gipa, *pInstance, "vkCreateDevice");
    data->dispatch.EnumeratePhysicalDevices =
        getProc<PFN_vkEnumeratePhysicalDevices>(gipa, *pInstance, "vkEnumeratePhysicalDevices");
    data->dispatch.GetPhysicalDeviceMemoryProperties =
        getProc<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, *pInstance, "vkGetPhysicalDeviceMemoryProperties");
    data->dispatch.GetPhysicalDeviceQueueFamilyProperties = getProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa, *pInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    data->dispatch.GetPhysicalDeviceSurfaceFormatsKHR = getProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        gipa, *pInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    data->dispatch.EnumerateDeviceExtensionProperties = getProc<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa, *pInstance, "vkEnumerateDeviceExtensionProperties");
    data->dispatch.DestroySurfaceKHR = getProc<PFN_vkDestroySurfaceKHR>(gipa, *pInstance, "vkDestroySurfaceKHR");
    data->dispatch.CreateWaylandSurfaceKHR =
        getProc<PFN_vkCreateWaylandSurfaceKHR>(gipa, *pInstance, "vkCreateWaylandSurfaceKHR");
    data->dispatch.CreateXlibSurfaceKHR =
        getProc<PFN_vkCreateXlibSurfaceKHR>(gipa, *pInstance, "vkCreateXlibSurfaceKHR");
    data->dispatch.CreateXcbSurfaceKHR =
        getProc<PFN_vkCreateXcbSurfaceKHR>(gipa, *pInstance, "vkCreateXcbSurfaceKHR");

    {
        std::lock_guard lock(globalMutex());
        instanceMap()[reinterpret_cast<uintptr_t>(*pInstance)] = data;
    }

    logf("instance created; effect active=%d exe=%s", isEffectActiveForCurrentProcess() ? 1 : 0,
         currentExecutableName().c_str());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    InstanceData *data = getInstanceData(instance);
    PFN_vkDestroyInstance destroy = data ? data->dispatch.DestroyInstance : nullptr;
    if (data) {
        {
            std::lock_guard lock(globalMutex());
            instanceMap().erase(reinterpret_cast<uintptr_t>(instance));
        }
        delete data;
    }
    if (destroy) {
        destroy(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *layerCreateInfo = nullptr;
    for (auto *node = static_cast<const VkBaseInStructure *>(pCreateInfo->pNext); node;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto *candidate = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<VkBaseInStructure *>(node));
            if (candidate->function == VK_LAYER_LINK_INFO) {
                layerCreateInfo = candidate;
                break;
            }
        }
    }
    if (!layerCreateInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    // Find owning instance via physical device enumeration — store through a side map.
    // Loader passes instance via GetInstanceProcAddr; find InstanceData by scanning.
    InstanceData *instanceData = nullptr;
    {
        std::lock_guard lock(globalMutex());
        for (auto &entry : instanceMap()) {
            instanceData = entry.second;
            break; // Prefer last/only; multi-instance rare for games.
        }
        // Better: match physicalDevice to instance by EnumeratePhysicalDevices.
        for (auto &entry : instanceMap()) {
            uint32_t count = 0;
            entry.second->dispatch.EnumeratePhysicalDevices(entry.second->instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            entry.second->dispatch.EnumeratePhysicalDevices(entry.second->instance, &count, devices.data());
            if (std::find(devices.begin(), devices.end(), physicalDevice) != devices.end()) {
                instanceData = entry.second;
                break;
            }
        }
    }
    if (!instanceData) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instanceData->instance, "vkCreateDevice"));

    // Ensure swapchain extension remains (app should already request it).
    const VkResult result = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    auto *dev = new DeviceData();
    dev->device = *pDevice;
    dev->physicalDevice = physicalDevice;
    dev->instance = instanceData;
    dev->dispatch.GetDeviceProcAddr = gdpa;
    auto &dd = dev->dispatch;
#define LOAD(name) dd.name = getProc<PFN_vk##name>(gdpa, *pDevice, "vk" #name)
    LOAD(DestroyDevice);
    LOAD(CreateSwapchainKHR);
    LOAD(DestroySwapchainKHR);
    LOAD(GetSwapchainImagesKHR);
    LOAD(QueuePresentKHR);
    LOAD(CreateImage);
    LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindImageMemory);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(CreateSampler);
    LOAD(DestroySampler);
    LOAD(CreateDescriptorSetLayout);
    LOAD(DestroyDescriptorSetLayout);
    LOAD(CreatePipelineLayout);
    LOAD(DestroyPipelineLayout);
    LOAD(CreateShaderModule);
    LOAD(DestroyShaderModule);
    LOAD(CreateComputePipelines);
    LOAD(DestroyPipeline);
    LOAD(CreateDescriptorPool);
    LOAD(DestroyDescriptorPool);
    LOAD(AllocateDescriptorSets);
    LOAD(UpdateDescriptorSets);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(FreeCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdBindPipeline);
    LOAD(CmdBindDescriptorSets);
    LOAD(CmdDispatch);
    LOAD(CmdCopyImage);
    LOAD(CmdBlitImage);
    LOAD(CmdFillBuffer);
    LOAD(QueueSubmit);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateSemaphore);
    LOAD(DestroySemaphore);
    LOAD(GetDeviceQueue);
#undef LOAD
    dd.SetHdrMetadataEXT = getProc<PFN_vkSetHdrMetadataEXT>(gdpa, *pDevice, "vkSetHdrMetadataEXT");
    dev->hdrMetadataExt = dd.SetHdrMetadataEXT != nullptr;

    // Prefer a queue family with graphics+compute (apps almost always create one).
    uint32_t qCount = 0;
    instanceData->dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    instanceData->dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, qprops.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            dev->graphicsQueueFamily = i;
            break;
        }
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            dev->graphicsQueueFamily = i;
        }
    }
    if (pCreateInfo->queueCreateInfoCount > 0) {
        // Prefer the queue family the app actually created.
        dev->graphicsQueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }

    {
        std::lock_guard lock(globalMutex());
        deviceMap()[reinterpret_cast<uintptr_t>(*pDevice)] = dev;
    }
    logf("device created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    DeviceData *dev = getDeviceData(device);
    PFN_vkDestroyDevice destroy = nullptr;
    if (dev) {
        destroy = dev->dispatch.DestroyDevice;
        {
            std::lock_guard lock(dev->mutex);
            for (auto &entry : dev->swapchains) {
                destroySwapchainResources(dev, entry.second);
            }
            dev->swapchains.clear();
            destroyDeviceResources(dev);
        }
        {
            std::lock_guard lock(globalMutex());
            deviceMap().erase(reinterpret_cast<uintptr_t>(device));
        }
        delete dev;
    }
    if (destroy) {
        destroy(device, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                  const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    DeviceData *dev = getDeviceData(device);
    if (!dev || !dev->dispatch.CreateSwapchainKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSwapchainCreateInfoKHR info = *pCreateInfo;
    info.imageUsage = static_cast<VkImageUsageFlags>(info.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                     | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                                     | VK_IMAGE_USAGE_SAMPLED_BIT);

    // Prefer HDR10 colorspace when available and configured.
    if (wantPreferHdrSwapchain() && pCreateInfo->surface
        && dev->instance->dispatch.GetPhysicalDeviceSurfaceFormatsKHR) {
        uint32_t formatCount = 0;
        dev->instance->dispatch.GetPhysicalDeviceSurfaceFormatsKHR(dev->physicalDevice, pCreateInfo->surface,
                                                                   &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        dev->instance->dispatch.GetPhysicalDeviceSurfaceFormatsKHR(dev->physicalDevice, pCreateInfo->surface,
                                                                   &formatCount, formats.data());
        for (const auto &fmt : formats) {
            if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
                && (fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32
                    || fmt.format == VK_FORMAT_R16G16B16A16_SFLOAT || fmt.format == pCreateInfo->imageFormat)) {
                // Keep app format when possible; only switch colorspace if format matches.
                if (fmt.format == pCreateInfo->imageFormat || fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                    || fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
                    info.imageFormat = fmt.format;
                    info.imageColorSpace = fmt.colorSpace;
                    logf("prefer HDR10 swapchain format=%u", static_cast<unsigned>(fmt.format));
                    break;
                }
            }
        }
    }

    const VkResult result = dev->dispatch.CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Always create GPU resources so Super+H overlay works mid-session.
    SwapchainData sc{};
    sc.swapchain = *pSwapchain;
    sc.device = device;
    sc.format = info.imageFormat;
    sc.colorSpace = info.imageColorSpace;
    sc.extent = info.imageExtent;
    sc.queueFamily = dev->graphicsQueueFamily;
    sc.hdrColorspace = isHdrColorSpace(info.imageColorSpace);
    sc.encoding = resolveEncoding(info.imageColorSpace);
    // UNORM/packed formats store sRGB-encoded bytes from typical SDR games.
    // *_SRGB formats are linearized by the sampler, so skip shader sRGB decode.
    const bool formatIsSrgb = (info.imageFormat == VK_FORMAT_R8G8B8A8_SRGB
                               || info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB
                               || info.imageFormat == VK_FORMAT_A8B8G8R8_SRGB_PACK32);
    sc.inputIsSrgb = !formatIsSrgb && !sc.hdrColorspace;
    if (sc.encoding == OutputEncoding::SdrPreview && !formatIsSrgb) {
        sc.inputIsSrgb = true;
    }
    // When we remapped to HDR10 but the app still draws SDR into the image, decode sRGB.
    if (sc.hdrColorspace && sc.encoding == OutputEncoding::Pq && !formatIsSrgb) {
        sc.inputIsSrgb = true;
    }

    std::lock_guard lock(dev->mutex);
    if (!createSwapchainResources(dev, sc)) {
        logf("failed to create swapchain resources; passthrough present");
        sc.active = false;
    }
    dev->swapchains[reinterpret_cast<uint64_t>(*pSwapchain)] = std::move(sc);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks *pAllocator)
{
    DeviceData *dev = getDeviceData(device);
    if (dev) {
        std::lock_guard lock(dev->mutex);
        auto it = dev->swapchains.find(reinterpret_cast<uint64_t>(swapchain));
        if (it != dev->swapchains.end()) {
            destroySwapchainResources(dev, it->second);
            dev->swapchains.erase(it);
        }
        if (dev->dispatch.DestroySwapchainKHR) {
            dev->dispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    // Find device owning this queue by scanning — store queue->device map would be better.
    DeviceData *dev = nullptr;
    {
        std::lock_guard lock(globalMutex());
        for (auto &entry : deviceMap()) {
            if (entry.second->graphicsQueue == queue || entry.second->graphicsQueue == VK_NULL_HANDLE) {
                // Resolve queue if needed
                if (entry.second->graphicsQueue == VK_NULL_HANDLE && entry.second->dispatch.GetDeviceQueue) {
                    entry.second->dispatch.GetDeviceQueue(entry.second->device, entry.second->graphicsQueueFamily, 0,
                                                          &entry.second->graphicsQueue);
                }
            }
            if (entry.second->graphicsQueue == queue) {
                dev = entry.second;
                break;
            }
        }
        // Fallback: first device (common for single-GPU games). Also match any known queue by probing.
        if (!dev && !deviceMap().empty()) {
            // Present queue might not be graphics queue 0 — still try first device's swapchains.
            dev = deviceMap().begin()->second;
        }
    }

    if (!dev || !pPresentInfo || pPresentInfo->swapchainCount == 0) {
        if (dev && dev->dispatch.QueuePresentKHR) {
            return dev->dispatch.QueuePresentKHR(queue, pPresentInfo);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Resolve extent/encoding for hotkey UI from first known swapchain.
    uint32_t uiW = 0;
    uint32_t uiH = 0;
    float uiOutputMode = 0.0f;
    {
        std::lock_guard lock(dev->mutex);
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto it = dev->swapchains.find(reinterpret_cast<uint64_t>(pPresentInfo->pSwapchains[i]));
            if (it != dev->swapchains.end() && it->second.active) {
                uiW = it->second.extent.width;
                uiH = it->second.extent.height;
                uiOutputMode = encodingToOutputMode(it->second.encoding);
                break;
            }
        }
    }
    updateRuntimeUi(uiW, uiH, uiOutputMode);

    if (!shouldProcessPresent()) {
        return dev->dispatch.QueuePresentKHR(queue, pPresentInfo);
    }

    const bool effectOn = runtimeEffectOn();
    const bool drawOverlay = overlayVisible();

    VkPresentInfoKHR present = *pPresentInfo;
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkSwapchainKHR> swapchains;
    std::vector<uint32_t> imageIndices;
    std::vector<VkResult> results;

    waitSemaphores.reserve(pPresentInfo->swapchainCount);
    swapchains.reserve(pPresentInfo->swapchainCount);
    imageIndices.reserve(pPresentInfo->swapchainCount);

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        const VkSwapchainKHR scHandle = pPresentInfo->pSwapchains[i];
        const uint32_t imageIndex = pPresentInfo->pImageIndices[i];

        std::lock_guard lock(dev->mutex);
        auto it = dev->swapchains.find(reinterpret_cast<uint64_t>(scHandle));
        if (it == dev->swapchains.end() || !it->second.active) {
            swapchains.push_back(scHandle);
            imageIndices.push_back(imageIndex);
            continue;
        }

        VkSemaphore signal = VK_NULL_HANDLE;
        const bool ok = processPresent(dev, it->second, imageIndex, queue, pPresentInfo->waitSemaphoreCount,
                                       pPresentInfo->pWaitSemaphores, signal, effectOn, drawOverlay);
        if (!ok) {
            swapchains.push_back(scHandle);
            imageIndices.push_back(imageIndex);
            continue;
        }

        // After processing, present should wait on our done semaphore instead of original waits.
        waitSemaphores.push_back(signal);
        swapchains.push_back(scHandle);
        imageIndices.push_back(imageIndex);
    }

    present.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    present.pWaitSemaphores = waitSemaphores.empty() ? pPresentInfo->pWaitSemaphores : waitSemaphores.data();
    // If we processed all with new waits, drop original waits (already consumed by submit).
    if (!waitSemaphores.empty() && waitSemaphores.size() == pPresentInfo->swapchainCount) {
        // ok
    } else if (waitSemaphores.empty()) {
        present.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        present.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    }

    present.swapchainCount = static_cast<uint32_t>(swapchains.size());
    present.pSwapchains = swapchains.data();
    present.pImageIndices = imageIndices.data();
    if (pPresentInfo->pResults) {
        results.resize(swapchains.size());
        present.pResults = results.data();
    }

    const VkResult result = dev->dispatch.QueuePresentKHR(queue, &present);
    if (pPresentInfo->pResults && !results.empty()) {
        for (size_t i = 0; i < results.size() && i < pPresentInfo->swapchainCount; ++i) {
            pPresentInfo->pResults[i] = results[i];
        }
    }
    return result;
}

PFN_vkVoidFunction getInstanceProcAddrImpl(VkInstance instance, const char *pName);
PFN_vkVoidFunction getDeviceProcAddrImpl(VkDevice device, const char *pName);

PFN_vkVoidFunction getInstanceProcAddrImpl(VkInstance instance, const char *pName)
{
    if (!pName) {
        return nullptr;
    }
    const std::string name(pName);
    if (name == "vkCreateInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    }
    if (name == "vkDestroyInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    }
    if (name == "vkCreateDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    }
    if (name == "vkCreateWaylandSurfaceKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateWaylandSurfaceKHR);
    }
    if (name == "vkCreateXlibSurfaceKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateXlibSurfaceKHR);
    }
    if (name == "vkCreateXcbSurfaceKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateXcbSurfaceKHR);
    }
    if (name == "vkDestroySurfaceKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroySurfaceKHR);
    }
    if (name == "vkGetInstanceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(getInstanceProcAddrImpl);
    }
    if (name == "vkGetDeviceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(getDeviceProcAddrImpl);
    }

    InstanceData *data = instance ? getInstanceData(instance) : nullptr;
    if (data && data->dispatch.GetInstanceProcAddr) {
        return data->dispatch.GetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

PFN_vkVoidFunction getDeviceProcAddrImpl(VkDevice device, const char *pName)
{
    if (!pName) {
        return nullptr;
    }
    const std::string name(pName);
    if (name == "vkGetDeviceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(getDeviceProcAddrImpl);
    }
    if (name == "vkDestroyDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    }
    if (name == "vkCreateSwapchainKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(CreateSwapchainKHR);
    }
    if (name == "vkDestroySwapchainKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroySwapchainKHR);
    }
    if (name == "vkQueuePresentKHR") {
        return reinterpret_cast<PFN_vkVoidFunction>(QueuePresentKHR);
    }

    DeviceData *data = device ? getDeviceData(device) : nullptr;
    if (data && data->dispatch.GetDeviceProcAddr) {
        return data->dispatch.GetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

} // namespace

PFN_vkVoidFunction getInstanceProcAddr(VkInstance instance, const char *pName)
{
    return getInstanceProcAddrImpl(instance, pName);
}

PFN_vkVoidFunction getDeviceProcAddr(VkDevice device, const char *pName)
{
    return getDeviceProcAddrImpl(device, pName);
}

} // namespace AutoHdrVk

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return AutoHdrVk::getInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return AutoHdrVk::getDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1) {
        return VK_INCOMPLETE;
    }
    std::memset(pProperties, 0, sizeof(VkLayerProperties));
    std::strncpy(pProperties[0].layerName, AutoHdrVk::kLayerName, VK_MAX_EXTENSION_NAME_SIZE - 1);
    std::strncpy(pProperties[0].description, AutoHdrVk::kLayerDescription, VK_MAX_DESCRIPTION_SIZE - 1);
    pProperties[0].implementationVersion = 1;
    pProperties[0].specVersion = VK_API_VERSION_1_3;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                                                                      uint32_t *pPropertyCount,
                                                                                      VkExtensionProperties *)
{
    if (!pLayerName || std::strcmp(pLayerName, AutoHdrVk::kLayerName) != 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice,
                                                                                uint32_t *pPropertyCount,
                                                                                VkLayerProperties *pProperties)
{
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice,
                                                                                    const char *pLayerName,
                                                                                    uint32_t *pPropertyCount,
                                                                                    VkExtensionProperties *)
{
    if (!pLayerName || std::strcmp(pLayerName, AutoHdrVk::kLayerName) != 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

} // extern "C"
