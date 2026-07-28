#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR

#include "config.hpp"
#include "tone_curve.hpp"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AutoHdrVk {

constexpr const char *kLayerName = "VK_LAYER_AUTOHDR_tonemap";
constexpr const char *kLayerDescription = "AutoHDR SDR to HDR tone mapping layer";

struct InstanceData;

inline void logf(const char *fmt, ...)
{
    if (const char *env = std::getenv("AUTOHDR_LOG"); env && *env && std::string(env) != "0") {
        fprintf(stderr, "[autohdr-vk] ");
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkCreateWaylandSurfaceKHR CreateWaylandSurfaceKHR = nullptr;
    PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR = nullptr;
    PFN_vkCreateXcbSurfaceKHR CreateXcbSurfaceKHR = nullptr;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCmdFillBuffer CmdFillBuffer = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkSetHdrMetadataEXT SetHdrMetadataEXT = nullptr;
};

// std140 layout — must match shaders/tonemap.comp ToneParams
struct ToneParamsUBO {
    float blackPoint = 0.0f;
    float colorIntensity = 0.33f;
    float gamutExpansion = 1.5f;
    float referenceNits = 203.0f;
    float peakNits = 1000.0f;
    float toneCurveInputSpan = 203.0f;
    float highlightSoftness = 0.30f;
    float perceptualColorEnabled = 1.0f;
    float outputMode = 0.0f;
    float inputIsSrgb = 1.0f;
    float intensity = 0.5f;
    float ditherStrength = 1.0f;
    float pqBoostParams[4] = {10000.0f, 10000.0f, 1.0f, 0.0f};
    uint32_t extentWidth = 0;
    uint32_t extentHeight = 0;
    uint32_t outputBits = 10;
    uint32_t _padBits = 0;
};

// std140 layout — must match shaders/overlay.comp OverlayParams
struct OverlayParamsUBO {
    uint32_t extentWidth = 0;
    uint32_t extentHeight = 0;
    float intensity = 0.5f;
    float colorIntensity = 0.33f;
    float expansionShape = 0.55f;
    float focused = 0.0f;
    float outputMode = 0.0f;
    float panelNits = 203.0f;
    float pointerValid = 0.0f;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
};

struct HistParamsUBO {
    uint32_t extentWidth = 0;
    uint32_t extentHeight = 0;
    float inputIsSrgb = 1.0f;
    float referenceNits = 203.0f;
};

struct AdaptParamsUBO {
    float intensity = 0.5f;
    float referenceNits = 203.0f;
    float peakNits = 1000.0f;
    float sdrWhiteNits = 100.0f;
    float adaptBrighten = 0.25f;
    float adaptDarken = 0.05f;
    float temporalEnable = 1.0f;
    float _pad = 0.0f;
};

struct UiParamsUBO {
    uint32_t extentWidth = 0;
    uint32_t extentHeight = 0;
    float inputIsSrgb = 1.0f;
    float pad = 0.0f;
};

struct BlurParamsUBO {
    uint32_t fullW = 0;
    uint32_t fullH = 0;
    uint32_t halfW = 0;
    uint32_t halfH = 0;
    float inputIsSrgb = 1.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};

struct SceneStatsGPU {
    float geoMean = 0.18f;
    float p10 = 0.05f;
    float p90 = 0.45f;
    float k1 = 0.83f;
    float effectivePeak = 1000.0f;
    float exposure = 1.0f;
    float maxCLL = 1000.0f;
    float maxFALL = 203.0f;
    uint32_t initialized = 0;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;
};

struct SwapchainImageResources {
    VkImage swapImage = VK_NULL_HANDLE;
    VkImage srcImage = VK_NULL_HANDLE; // copy of swap for sampling
    VkDeviceMemory srcMemory = VK_NULL_HANDLE;
    VkImageView srcView = VK_NULL_HANDLE;
    VkImage dstImage = VK_NULL_HANDLE; // rgba16f compute output
    VkDeviceMemory dstMemory = VK_NULL_HANDLE;
    VkImageView dstView = VK_NULL_HANDLE;
    VkDescriptorSet tonemapSet = VK_NULL_HANDLE;
    VkDescriptorSet histPass1Set = VK_NULL_HANDLE;
    VkDescriptorSet histPass2Set = VK_NULL_HANDLE;
    VkDescriptorSet uiClusterSet = VK_NULL_HANDLE;
    VkDescriptorSet baseBlurSet = VK_NULL_HANDLE;
    VkDescriptorSet overlaySet = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore doneSemaphore = VK_NULL_HANDLE;
};

struct SwapchainData {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkExtent2D halfExtent{};
    VkImage baseImage = VK_NULL_HANDLE;
    VkDeviceMemory baseMemory = VK_NULL_HANDLE;
    VkImageView baseView = VK_NULL_HANDLE;
    VkImage maskImage = VK_NULL_HANDLE;
    VkDeviceMemory maskMemory = VK_NULL_HANDLE;
    VkImageView maskView = VK_NULL_HANDLE;
    VkImageLayout maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout baseLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t queueFamily = 0;
    bool active = false;
    bool hdrColorspace = false;
    bool inputIsSrgb = true;
    OutputEncoding encoding = OutputEncoding::SdrPreview;

    VkBuffer histBuffer = VK_NULL_HANDLE;
    VkDeviceMemory histMemory = VK_NULL_HANDLE;
    VkBuffer sceneStatsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sceneStatsMemory = VK_NULL_HANDLE;
    void *sceneStatsMapped = nullptr;

    std::vector<SwapchainImageResources> images;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    InstanceData *instance = nullptr;
    DeviceDispatch dispatch{};
    uint32_t graphicsQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout tonemapSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout tonemapPipelineLayout = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout histPass1SetLayout = VK_NULL_HANDLE;
    VkPipelineLayout histPass1PipelineLayout = VK_NULL_HANDLE;
    VkPipeline histPass1Pipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout histPass2SetLayout = VK_NULL_HANDLE;
    VkPipelineLayout histPass2PipelineLayout = VK_NULL_HANDLE;
    VkPipeline histPass2Pipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout uiClusterSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout uiClusterPipelineLayout = VK_NULL_HANDLE;
    VkPipeline uiClusterPipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout baseBlurSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout baseBlurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline baseBlurPipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout = VK_NULL_HANDLE;
    VkPipeline overlayPipeline = VK_NULL_HANDLE;

    VkBuffer lutBuffer = VK_NULL_HANDLE;
    VkDeviceMemory lutMemory = VK_NULL_HANDLE;
    void *lutMapped = nullptr;

    VkBuffer uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    void *uboMapped = nullptr;

    VkBuffer histParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory histParamsMemory = VK_NULL_HANDLE;
    void *histParamsMapped = nullptr;

    VkBuffer adaptParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory adaptParamsMemory = VK_NULL_HANDLE;
    void *adaptParamsMapped = nullptr;

    VkBuffer uiParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uiParamsMemory = VK_NULL_HANDLE;
    void *uiParamsMapped = nullptr;

    VkBuffer blurParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blurParamsMemory = VK_NULL_HANDLE;
    void *blurParamsMapped = nullptr;

    VkBuffer overlayParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory overlayParamsMemory = VK_NULL_HANDLE;
    void *overlayParamsMapped = nullptr;

    bool pipelineReady = false;
    bool hdrMetadataExt = false;

    std::unordered_map<uint64_t, SwapchainData> swapchains;
    std::mutex mutex;
};

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    InstanceDispatch dispatch{};
    std::mutex mutex;
};

std::mutex &globalMutex();
std::unordered_map<uintptr_t, InstanceData *> &instanceMap();
std::unordered_map<uintptr_t, DeviceData *> &deviceMap();

InstanceData *getInstanceData(VkInstance instance);
DeviceData *getDeviceData(VkDevice device);

uint32_t findMemoryType(DeviceData *dev, uint32_t typeBits, VkMemoryPropertyFlags props);

bool ensureDeviceResources(DeviceData *dev);
void destroyDeviceResources(DeviceData *dev);
bool createSwapchainResources(DeviceData *dev, SwapchainData &sc);
void destroySwapchainResources(DeviceData *dev, SwapchainData &sc);
bool processPresent(DeviceData *dev, SwapchainData &sc, uint32_t imageIndex, VkQueue queue,
                    uint32_t waitCount, const VkSemaphore *waitSemaphores, VkSemaphore &outSignal,
                    bool effectOn, bool drawOverlay);
void uploadToneParams(DeviceData *dev, const AutoHdr::CalibrationSettings &settings, OutputEncoding encoding,
                      bool inputIsSrgb, VkExtent2D extent, VkExtent2D halfExtent, VkFormat swapFormat);
void uploadOverlayParams(DeviceData *dev, const OverlayParamsUBO &params);

} // namespace AutoHdrVk
