#pragma once

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
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
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
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
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
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
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
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
    float pqBoostParams[4] = {10000.0f, 10000.0f, 1.0f, 0.0f};
};

struct SwapchainImageResources {
    VkImage swapImage = VK_NULL_HANDLE;
    VkImageView swapView = VK_NULL_HANDLE; // sampled source after copy to work? we sample workSrc
    VkImage srcImage = VK_NULL_HANDLE;     // copy of swap content for sampling
    VkDeviceMemory srcMemory = VK_NULL_HANDLE;
    VkImageView srcView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE; // renders into swapView
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
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
    uint32_t queueFamily = 0;
    bool active = false;
    bool hdrColorspace = false;
    bool inputIsSrgb = true;
    OutputEncoding encoding = OutputEncoding::SdrPreview;
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
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat renderPassFormat = VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkBuffer lutBuffer = VK_NULL_HANDLE;
    VkDeviceMemory lutMemory = VK_NULL_HANDLE;
    void *lutMapped = nullptr;

    VkBuffer uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    void *uboMapped = nullptr;

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

bool ensureDeviceResources(DeviceData *dev, VkFormat swapFormat);
void destroyDeviceResources(DeviceData *dev);
bool createSwapchainResources(DeviceData *dev, SwapchainData &sc);
void destroySwapchainResources(DeviceData *dev, SwapchainData &sc);
bool processPresent(DeviceData *dev, SwapchainData &sc, uint32_t imageIndex, VkQueue queue,
                    uint32_t waitCount, const VkSemaphore *waitSemaphores, VkSemaphore &outSignal);
void uploadToneParams(DeviceData *dev, const AutoHdr::CalibrationSettings &settings, OutputEncoding encoding,
                      bool inputIsSrgb);

} // namespace AutoHdrVk
