#include "layer_common.hpp"

#include "calibration.hpp"
#include "runtime_ui.hpp"
#include "tone_curve.hpp"

#include "base_blur.comp.spv.h"
#include "histogram_pass1.comp.spv.h"
#include "histogram_pass2.comp.spv.h"
#include "overlay.comp.spv.h"
#include "overlay_font_atlas.h"
#include "present_rgb10.comp.spv.h"
#include "present_unorm.comp.spv.h"
#include "tonemap.comp.spv.h"
#include "ui_cluster.comp.spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AutoHdrVk {

namespace {

constexpr VkFormat kComputeDstFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kBaseFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMaskFormat = VK_FORMAT_R8_UNORM;
constexpr VkDeviceSize kHistBinCount = 64;
constexpr VkDeviceSize kHistBufferSize = kHistBinCount * sizeof(uint32_t);

std::mutex g_globalMutex;
std::unordered_map<uintptr_t, InstanceData *> g_instanceMap;
std::unordered_map<uintptr_t, DeviceData *> g_deviceMap;

VkShaderModule createShaderModule(DeviceData *dev, const uint32_t *code, size_t nbytes)
{
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = nbytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (dev->dispatch.CreateShaderModule(dev->device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool createHostBuffer(DeviceData *dev, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer,
                      VkDeviceMemory &memory, void **mapped)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dev->dispatch.CreateBuffer(dev->device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    dev->dispatch.GetBufferMemoryRequirements(dev->device, buffer, &req);
    const uint32_t type = findMemoryType(dev, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (dev->dispatch.AllocateMemory(dev->device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    if (dev->dispatch.BindBufferMemory(dev->device, buffer, memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (mapped) {
        if (dev->dispatch.MapMemory(dev->device, memory, 0, size, 0, mapped) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool createDeviceBuffer(DeviceData *dev, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer,
                        VkDeviceMemory &memory)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dev->dispatch.CreateBuffer(dev->device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    dev->dispatch.GetBufferMemoryRequirements(dev->device, buffer, &req);
    const uint32_t type = findMemoryType(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (dev->dispatch.AllocateMemory(dev->device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    return dev->dispatch.BindBufferMemory(dev->device, buffer, memory, 0) == VK_SUCCESS;
}

bool createDeviceImage(DeviceData *dev, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImage &image,
                       VkDeviceMemory &memory, VkImageView &view)
{
    auto &d = dev->dispatch;
    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = {extent.width, extent.height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.CreateImage(dev->device, &imgInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    d.GetImageMemoryRequirements(dev->device, image, &req);
    const uint32_t type = findMemoryType(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (d.AllocateMemory(dev->device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    d.BindImageMemory(dev->device, image, memory, 0);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    return d.CreateImageView(dev->device, &viewInfo, nullptr, &view) == VK_SUCCESS;
}

bool submitOneTimeCommands(DeviceData *dev, VkCommandBuffer cmd)
{
    auto &d = dev->dispatch;
    if (dev->graphicsQueue == VK_NULL_HANDLE) {
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (d.CreateFence(dev->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        return false;
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const VkResult submitResult = d.QueueSubmit(dev->graphicsQueue, 1, &submit, fence);
    if (submitResult != VK_SUCCESS) {
        d.DestroyFence(dev->device, fence, nullptr);
        return false;
    }
    if (d.WaitForFences(dev->device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        d.DestroyFence(dev->device, fence, nullptr);
        return false;
    }
    d.DestroyFence(dev->device, fence, nullptr);
    return true;
}

bool uploadOptimalSampledRgbaImage(DeviceData *dev, VkExtent2D extent, const uint8_t *pixels, VkDeviceSize rowBytes,
                                   VkImage &image, VkDeviceMemory &memory, VkImageView &view, VkImageLayout &outLayout)
{
    auto &d = dev->dispatch;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;

    if (!createDeviceImage(dev, extent, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, image, memory, view)) {
        return false;
    }

    const VkDeviceSize tightRowBytes = static_cast<VkDeviceSize>(extent.width * 4u);
    const VkDeviceSize bufferSize = tightRowBytes * extent.height;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void *stagingMapped = nullptr;
    if (!createHostBuffer(dev, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingBuffer, stagingMemory,
                          &stagingMapped)) {
        d.DestroyImageView(dev->device, view, nullptr);
        d.DestroyImage(dev->device, image, nullptr);
        d.FreeMemory(dev->device, memory, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        return false;
    }

    auto *dst = static_cast<uint8_t *>(stagingMapped);
    for (uint32_t y = 0; y < extent.height; ++y) {
        std::memcpy(dst + y * tightRowBytes, pixels + y * rowBytes, static_cast<size_t>(tightRowBytes));
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAlloc.commandPool = dev->commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (d.AllocateCommandBuffers(dev->device, &cmdAlloc, &cmd) != VK_SUCCESS) {
        d.UnmapMemory(dev->device, stagingMemory);
        d.DestroyBuffer(dev->device, stagingBuffer, nullptr);
        d.FreeMemory(dev->device, stagingMemory, nullptr);
        d.DestroyImageView(dev->device, view, nullptr);
        d.DestroyImage(dev->device, image, nullptr);
        d.FreeMemory(dev->device, memory, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        return false;
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    d.CmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    d.EndCommandBuffer(cmd);
    const bool ok = submitOneTimeCommands(dev, cmd);
    d.FreeCommandBuffers(dev->device, dev->commandPool, 1, &cmd);
    d.UnmapMemory(dev->device, stagingMemory);
    d.DestroyBuffer(dev->device, stagingBuffer, nullptr);
    d.FreeMemory(dev->device, stagingMemory, nullptr);

    if (!ok) {
        d.DestroyImageView(dev->device, view, nullptr);
        d.DestroyImage(dev->device, image, nullptr);
        d.FreeMemory(dev->device, memory, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        return false;
    }

    outLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool uploadOverlayFontAtlas(DeviceData *dev)
{
    if (dev->overlayFontView != VK_NULL_HANDLE) {
        return true;
    }
    if (dev->graphicsQueue == VK_NULL_HANDLE) {
        return false;
    }

    const VkExtent2D extent{kOverlayFontAtlasWidth, kOverlayFontAtlasHeight};
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(extent.width * 4u);
    if (uploadOptimalSampledRgbaImage(dev, extent, kOverlayFontAtlasRgba, rowBytes, dev->overlayFontImage,
                                      dev->overlayFontMemory, dev->overlayFontView, dev->overlayFontLayout)) {
        return true;
    }

    logf("overlay font atlas upload failed");
    static const uint8_t kFallbackPixel[4] = {255, 255, 255, 255};
    const VkExtent2D fallbackExtent{1, 1};
    return uploadOptimalSampledRgbaImage(dev, fallbackExtent, kFallbackPixel, 4, dev->overlayFontImage,
                                         dev->overlayFontMemory, dev->overlayFontView, dev->overlayFontLayout);
}

bool isRgb10SwapFormat(VkFormat format)
{
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

bool isUnorm8SwapFormat(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM
        || format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
}

uint32_t rgb10PackModeForFormat(VkFormat format)
{
    return format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? 1u : 0u;
}

bool unormBgraForFormat(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool createComputePipeline(DeviceData *dev, const uint32_t *spv, size_t spvSize, VkPipelineLayout layout,
                           VkPipeline &pipeline)
{
    auto &d = dev->dispatch;
    VkShaderModule module = createShaderModule(dev, spv, spvSize);
    if (!module) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = stage;
    cp.layout = layout;

    const VkResult result = d.CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
    d.DestroyShaderModule(dev->device, module, nullptr);
    return result == VK_SUCCESS;
}

} // namespace

std::mutex &globalMutex()
{
    return g_globalMutex;
}

std::unordered_map<uintptr_t, InstanceData *> &instanceMap()
{
    return g_instanceMap;
}

std::unordered_map<uintptr_t, DeviceData *> &deviceMap()
{
    return g_deviceMap;
}

InstanceData *getInstanceData(VkInstance instance)
{
    std::lock_guard lock(g_globalMutex);
    auto it = g_instanceMap.find(reinterpret_cast<uintptr_t>(instance));
    return it == g_instanceMap.end() ? nullptr : it->second;
}

DeviceData *getDeviceData(VkDevice device)
{
    std::lock_guard lock(g_globalMutex);
    auto it = g_deviceMap.find(reinterpret_cast<uintptr_t>(device));
    return it == g_deviceMap.end() ? nullptr : it->second;
}

uint32_t findMemoryType(DeviceData *dev, uint32_t typeBits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    dev->instance->dispatch.GetPhysicalDeviceMemoryProperties(dev->physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

void uploadToneParams(DeviceData *dev, const AutoHdr::CalibrationSettings &settings, OutputEncoding encoding,
                      bool inputIsSrgb, VkExtent2D extent, VkExtent2D halfExtent, VkFormat swapFormat)
{
    if (!dev->uboMapped || !dev->lutMapped) {
        return;
    }

    const float intensity = std::clamp(settings.intensity, 0.0f, 1.0f);

    AutoHdr::ToneCurveEndpoints endpoints;
    endpoints.peakNits = settings.maxNits;
    endpoints.visualReferenceNits = settings.referenceNits;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint.x > 0.0f ? settings.sdrMaxPoint
                                                          : AutoHdr::Vec2{settings.referenceNits, settings.maxNits};

    // expansion_shape drives the live LUT (linear→exponential morph); overrides static preset points.
    AutoHdr::PresetCurveParams shapeParams;
    shapeParams.referenceNits = settings.referenceNits;
    shapeParams.peakNits = settings.maxNits;
    const float shape = AutoHdr::clampExpansionShape(settings.expansionShape);
    const auto intermediates = AutoHdr::generateExpansionShapePoints(shape, shapeParams);
    const auto full = AutoHdr::buildFullCurve(endpoints, intermediates);
    const float span = std::max(endpoints.sdrMaxPoint.x, settings.referenceNits);
    float lut[AutoHdr::kToneCurveLutSize];
    AutoHdr::buildToneCurveLut(full, span, lut, AutoHdr::kToneCurveLutSize);
    float packed[256 * 4];
    for (int i = 0; i < AutoHdr::kToneCurveLutSize; ++i) {
        packed[i] = lut[i];
    }
    std::memcpy(dev->lutMapped, packed, sizeof(packed));

    uint32_t outputBits = 8;
    switch (swapFormat) {
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
        outputBits = 10;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
        outputBits = 16;
        break;
    default:
        outputBits = 8;
        break;
    }

    ToneParamsUBO ubo{};
    ubo.blackPoint = AutoHdr::blackFloorToBlackPoint(settings.blackFloor);
    ubo.colorIntensity = std::clamp(settings.colorIntensity, 0.0f, 1.0f);
    ubo.gamutExpansion = settings.gamutExpansion;
    ubo.referenceNits = settings.referenceNits;
    // User-configured peak — intensity is applied as a post-map mix in the shader.
    ubo.peakNits = settings.maxNits;
    ubo.toneCurveInputSpan = span;
    ubo.highlightSoftness = settings.highlightSoftness;
    ubo.perceptualColorEnabled = settings.perceptualColor ? 1.0f : 0.0f;
    ubo.inputIsSrgb = inputIsSrgb ? 1.0f : 0.0f;
    ubo.intensity = intensity;
    ubo.highlightStretch = AutoHdr::clampHighlightStretch(settings.highlightStretch);
    ubo.ditherStrength = settings.dither ? AutoHdr::clampDitherStrength(settings.ditherStrength) : 0.0f;
    switch (encoding) {
    case OutputEncoding::LinearScRgb:
        ubo.outputMode = 1.0f;
        break;
    case OutputEncoding::SdrPreview:
        ubo.outputMode = 2.0f;
        break;
    case OutputEncoding::Pq:
    case OutputEncoding::Auto:
        ubo.outputMode = 0.0f;
        break;
    }
    ubo.pqBoostParams[0] = 10000.0f;
    ubo.pqBoostParams[1] = 10000.0f;
    ubo.pqBoostParams[2] = 1.0f;
    ubo.pqBoostParams[3] = 0.0f;
    ubo.extentWidth = extent.width;
    ubo.extentHeight = extent.height;
    ubo.outputBits = outputBits;
    std::memcpy(dev->uboMapped, &ubo, sizeof(ubo));

    if (dev->histParamsMapped) {
        HistParamsUBO histParams{};
        histParams.extentWidth = extent.width;
        histParams.extentHeight = extent.height;
        histParams.inputIsSrgb = inputIsSrgb ? 1.0f : 0.0f;
        histParams.referenceNits = settings.referenceNits;
        std::memcpy(dev->histParamsMapped, &histParams, sizeof(histParams));
    }

    if (dev->adaptParamsMapped) {
        AdaptParamsUBO adaptParams{};
        adaptParams.intensity = intensity;
        adaptParams.referenceNits = settings.referenceNits;
        adaptParams.peakNits = settings.maxNits;
        adaptParams.sdrWhiteNits = 80.0f;
        // Temporal smoothing for geo-mean / p10 / p90 / p98 / k1 scene stats.
        adaptParams.adaptBrighten = 0.010f;
        adaptParams.adaptDarken = 0.015f;
        adaptParams.temporalEnable = 1.0f;
        std::memcpy(dev->adaptParamsMapped, &adaptParams, sizeof(adaptParams));
    }

    if (dev->uiParamsMapped) {
        UiParamsUBO uiParams{};
        uiParams.extentWidth = extent.width;
        uiParams.extentHeight = extent.height;
        uiParams.inputIsSrgb = inputIsSrgb ? 1.0f : 0.0f;
        std::memcpy(dev->uiParamsMapped, &uiParams, sizeof(uiParams));
    }

    if (dev->blurParamsMapped) {
        BlurParamsUBO blurParams{};
        blurParams.fullW = extent.width;
        blurParams.fullH = extent.height;
        blurParams.halfW = halfExtent.width;
        blurParams.halfH = halfExtent.height;
        blurParams.inputIsSrgb = inputIsSrgb ? 1.0f : 0.0f;
        std::memcpy(dev->blurParamsMapped, &blurParams, sizeof(blurParams));
    }
}

void uploadOverlayParams(DeviceData *dev, const OverlayParamsUBO &params)
{
    if (!dev->overlayParamsMapped) {
        return;
    }
    std::memcpy(dev->overlayParamsMapped, &params, sizeof(params));
}

void destroyDeviceResources(DeviceData *dev)
{
    if (!dev) {
        return;
    }
    auto &d = dev->dispatch;

    if (dev->tonemapPipeline) {
        d.DestroyPipeline(dev->device, dev->tonemapPipeline, nullptr);
        dev->tonemapPipeline = VK_NULL_HANDLE;
    }
    if (dev->histPass1Pipeline) {
        d.DestroyPipeline(dev->device, dev->histPass1Pipeline, nullptr);
        dev->histPass1Pipeline = VK_NULL_HANDLE;
    }
    if (dev->histPass2Pipeline) {
        d.DestroyPipeline(dev->device, dev->histPass2Pipeline, nullptr);
        dev->histPass2Pipeline = VK_NULL_HANDLE;
    }
    if (dev->uiClusterPipeline) {
        d.DestroyPipeline(dev->device, dev->uiClusterPipeline, nullptr);
        dev->uiClusterPipeline = VK_NULL_HANDLE;
    }
    if (dev->baseBlurPipeline) {
        d.DestroyPipeline(dev->device, dev->baseBlurPipeline, nullptr);
        dev->baseBlurPipeline = VK_NULL_HANDLE;
    }
    if (dev->overlayPipeline) {
        d.DestroyPipeline(dev->device, dev->overlayPipeline, nullptr);
        dev->overlayPipeline = VK_NULL_HANDLE;
    }
    if (dev->presentPipeline) {
        d.DestroyPipeline(dev->device, dev->presentPipeline, nullptr);
        dev->presentPipeline = VK_NULL_HANDLE;
    }
    if (dev->presentUnormPipeline) {
        d.DestroyPipeline(dev->device, dev->presentUnormPipeline, nullptr);
        dev->presentUnormPipeline = VK_NULL_HANDLE;
    }
    if (dev->tonemapPipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->tonemapPipelineLayout, nullptr);
        dev->tonemapPipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->histPass1PipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->histPass1PipelineLayout, nullptr);
        dev->histPass1PipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->histPass2PipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->histPass2PipelineLayout, nullptr);
        dev->histPass2PipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->uiClusterPipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->uiClusterPipelineLayout, nullptr);
        dev->uiClusterPipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->baseBlurPipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->baseBlurPipelineLayout, nullptr);
        dev->baseBlurPipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->overlayPipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->overlayPipelineLayout, nullptr);
        dev->overlayPipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->presentPipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->presentPipelineLayout, nullptr);
        dev->presentPipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->tonemapSetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->tonemapSetLayout, nullptr);
        dev->tonemapSetLayout = VK_NULL_HANDLE;
    }
    if (dev->histPass1SetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->histPass1SetLayout, nullptr);
        dev->histPass1SetLayout = VK_NULL_HANDLE;
    }
    if (dev->histPass2SetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->histPass2SetLayout, nullptr);
        dev->histPass2SetLayout = VK_NULL_HANDLE;
    }
    if (dev->uiClusterSetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->uiClusterSetLayout, nullptr);
        dev->uiClusterSetLayout = VK_NULL_HANDLE;
    }
    if (dev->baseBlurSetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->baseBlurSetLayout, nullptr);
        dev->baseBlurSetLayout = VK_NULL_HANDLE;
    }
    if (dev->overlaySetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->overlaySetLayout, nullptr);
        dev->overlaySetLayout = VK_NULL_HANDLE;
    }
    if (dev->presentSetLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->presentSetLayout, nullptr);
        dev->presentSetLayout = VK_NULL_HANDLE;
    }
    if (dev->fontSampler) {
        d.DestroySampler(dev->device, dev->fontSampler, nullptr);
        dev->fontSampler = VK_NULL_HANDLE;
    }
    if (dev->sampler) {
        d.DestroySampler(dev->device, dev->sampler, nullptr);
        dev->sampler = VK_NULL_HANDLE;
    }
    if (dev->descriptorPool) {
        d.DestroyDescriptorPool(dev->device, dev->descriptorPool, nullptr);
        dev->descriptorPool = VK_NULL_HANDLE;
    }
    if (dev->uboMapped) {
        d.UnmapMemory(dev->device, dev->uboMemory);
        dev->uboMapped = nullptr;
    }
    if (dev->uboBuffer) {
        d.DestroyBuffer(dev->device, dev->uboBuffer, nullptr);
        dev->uboBuffer = VK_NULL_HANDLE;
    }
    if (dev->uboMemory) {
        d.FreeMemory(dev->device, dev->uboMemory, nullptr);
        dev->uboMemory = VK_NULL_HANDLE;
    }
    if (dev->lutMapped) {
        d.UnmapMemory(dev->device, dev->lutMemory);
        dev->lutMapped = nullptr;
    }
    if (dev->lutBuffer) {
        d.DestroyBuffer(dev->device, dev->lutBuffer, nullptr);
        dev->lutBuffer = VK_NULL_HANDLE;
    }
    if (dev->lutMemory) {
        d.FreeMemory(dev->device, dev->lutMemory, nullptr);
        dev->lutMemory = VK_NULL_HANDLE;
    }
    if (dev->histParamsMapped) {
        d.UnmapMemory(dev->device, dev->histParamsMemory);
        dev->histParamsMapped = nullptr;
    }
    if (dev->histParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->histParamsBuffer, nullptr);
        dev->histParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->histParamsMemory) {
        d.FreeMemory(dev->device, dev->histParamsMemory, nullptr);
        dev->histParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->adaptParamsMapped) {
        d.UnmapMemory(dev->device, dev->adaptParamsMemory);
        dev->adaptParamsMapped = nullptr;
    }
    if (dev->adaptParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->adaptParamsBuffer, nullptr);
        dev->adaptParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->adaptParamsMemory) {
        d.FreeMemory(dev->device, dev->adaptParamsMemory, nullptr);
        dev->adaptParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->uiParamsMapped) {
        d.UnmapMemory(dev->device, dev->uiParamsMemory);
        dev->uiParamsMapped = nullptr;
    }
    if (dev->uiParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->uiParamsBuffer, nullptr);
        dev->uiParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->uiParamsMemory) {
        d.FreeMemory(dev->device, dev->uiParamsMemory, nullptr);
        dev->uiParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->blurParamsMapped) {
        d.UnmapMemory(dev->device, dev->blurParamsMemory);
        dev->blurParamsMapped = nullptr;
    }
    if (dev->blurParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->blurParamsBuffer, nullptr);
        dev->blurParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->blurParamsMemory) {
        d.FreeMemory(dev->device, dev->blurParamsMemory, nullptr);
        dev->blurParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->overlayParamsMapped) {
        d.UnmapMemory(dev->device, dev->overlayParamsMemory);
        dev->overlayParamsMapped = nullptr;
    }
    if (dev->overlayParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->overlayParamsBuffer, nullptr);
        dev->overlayParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->overlayParamsMemory) {
        d.FreeMemory(dev->device, dev->overlayParamsMemory, nullptr);
        dev->overlayParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->presentParamsMapped) {
        d.UnmapMemory(dev->device, dev->presentParamsMemory);
        dev->presentParamsMapped = nullptr;
    }
    if (dev->presentParamsBuffer) {
        d.DestroyBuffer(dev->device, dev->presentParamsBuffer, nullptr);
        dev->presentParamsBuffer = VK_NULL_HANDLE;
    }
    if (dev->presentParamsMemory) {
        d.FreeMemory(dev->device, dev->presentParamsMemory, nullptr);
        dev->presentParamsMemory = VK_NULL_HANDLE;
    }
    if (dev->overlayFontView) {
        d.DestroyImageView(dev->device, dev->overlayFontView, nullptr);
        dev->overlayFontView = VK_NULL_HANDLE;
    }
    if (dev->overlayFontImage) {
        d.DestroyImage(dev->device, dev->overlayFontImage, nullptr);
        dev->overlayFontImage = VK_NULL_HANDLE;
    }
    if (dev->overlayFontMemory) {
        d.FreeMemory(dev->device, dev->overlayFontMemory, nullptr);
        dev->overlayFontMemory = VK_NULL_HANDLE;
    }
    dev->overlayFontLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dev->commandPool) {
        d.DestroyCommandPool(dev->device, dev->commandPool, nullptr);
        dev->commandPool = VK_NULL_HANDLE;
    }
    dev->pipelineReady = false;
}

bool ensureDeviceResources(DeviceData *dev)
{
    if (dev->pipelineReady) {
        return true;
    }
    destroyDeviceResources(dev);

    auto &d = dev->dispatch;

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = dev->graphicsQueueFamily;
    if (d.CreateCommandPool(dev->device, &poolInfo, nullptr, &dev->commandPool) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d.CreateSampler(dev->device, &samp, nullptr, &dev->sampler) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo fontSamp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    fontSamp.magFilter = VK_FILTER_NEAREST;
    fontSamp.minFilter = VK_FILTER_NEAREST;
    fontSamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    fontSamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    fontSamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d.CreateSampler(dev->device, &fontSamp, nullptr, &dev->fontSampler) != VK_SUCCESS) {
        return false;
    }

    if (!createHostBuffer(dev, sizeof(ToneParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->uboBuffer,
                          dev->uboMemory, &dev->uboMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(float) * 4 * 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->lutBuffer,
                          dev->lutMemory, &dev->lutMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(HistParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->histParamsBuffer,
                          dev->histParamsMemory, &dev->histParamsMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(AdaptParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->adaptParamsBuffer,
                          dev->adaptParamsMemory, &dev->adaptParamsMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(UiParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->uiParamsBuffer,
                          dev->uiParamsMemory, &dev->uiParamsMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(BlurParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->blurParamsBuffer,
                          dev->blurParamsMemory, &dev->blurParamsMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(OverlayParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->overlayParamsBuffer,
                          dev->overlayParamsMemory, &dev->overlayParamsMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(PresentParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->presentParamsBuffer,
                          dev->presentParamsMemory, &dev->presentParamsMapped)) {
        return false;
    }

    {
        // Legacy color tonemap: input, ToneLut, mask, ToneParams, storage out, SceneStats
        VkDescriptorSetLayoutBinding bindings[6]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 6;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->tonemapSetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->histPass1SetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->histPass2SetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->uiClusterSetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->baseBlurSetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->overlaySetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (d.CreateDescriptorSetLayout(dev->device, &layoutInfo, nullptr, &dev->presentSetLayout) != VK_SUCCESS) {
            return false;
        }
    }

    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->tonemapSetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->tonemapPipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->histPass1SetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->histPass1PipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->histPass2SetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->histPass2PipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->uiClusterSetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->uiClusterPipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->baseBlurSetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->baseBlurPipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->overlaySetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->overlayPipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev->presentSetLayout;
        if (d.CreatePipelineLayout(dev->device, &layoutInfo, nullptr, &dev->presentPipelineLayout) != VK_SUCCESS) {
            return false;
        }
    }

    if (!createComputePipeline(dev, tonemap_comp_spv, sizeof(tonemap_comp_spv), dev->tonemapPipelineLayout,
                               dev->tonemapPipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, histogram_pass1_comp_spv, sizeof(histogram_pass1_comp_spv),
                               dev->histPass1PipelineLayout, dev->histPass1Pipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, histogram_pass2_comp_spv, sizeof(histogram_pass2_comp_spv),
                               dev->histPass2PipelineLayout, dev->histPass2Pipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, ui_cluster_comp_spv, sizeof(ui_cluster_comp_spv), dev->uiClusterPipelineLayout,
                               dev->uiClusterPipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, base_blur_comp_spv, sizeof(base_blur_comp_spv), dev->baseBlurPipelineLayout,
                               dev->baseBlurPipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, overlay_comp_spv, sizeof(overlay_comp_spv), dev->overlayPipelineLayout,
                               dev->overlayPipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, present_rgb10_comp_spv, sizeof(present_rgb10_comp_spv), dev->presentPipelineLayout,
                               dev->presentPipeline)) {
        return false;
    }
    if (!createComputePipeline(dev, present_unorm_comp_spv, sizeof(present_unorm_comp_spv), dev->presentPipelineLayout,
                               dev->presentUnormPipeline)) {
        return false;
    }

    d.GetDeviceQueue(dev->device, dev->graphicsQueueFamily, 0, &dev->graphicsQueue);
    if (!uploadOverlayFontAtlas(dev)) {
        logf("overlay font unavailable; labels may be missing");
    }

    VkDescriptorPoolSize sizes[4]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 512;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 512;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = 256;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[3].descriptorCount = 256;
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 512;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes;
    if (d.CreateDescriptorPool(dev->device, &dp, nullptr, &dev->descriptorPool) != VK_SUCCESS) {
        return false;
    }

    dev->pipelineReady = true;
    logf("compute pipelines ready (hist+ui+blur+legacyTone+overlay+present+presentUnorm)");
    return true;
}

void destroySwapchainResources(DeviceData *dev, SwapchainData &sc)
{
    auto &d = dev->dispatch;

    if (sc.sceneStatsMapped) {
        d.UnmapMemory(dev->device, sc.sceneStatsMemory);
        sc.sceneStatsMapped = nullptr;
    }
    if (sc.sceneStatsBuffer) {
        d.DestroyBuffer(dev->device, sc.sceneStatsBuffer, nullptr);
        sc.sceneStatsBuffer = VK_NULL_HANDLE;
    }
    if (sc.sceneStatsMemory) {
        d.FreeMemory(dev->device, sc.sceneStatsMemory, nullptr);
        sc.sceneStatsMemory = VK_NULL_HANDLE;
    }
    if (sc.histBuffer) {
        d.DestroyBuffer(dev->device, sc.histBuffer, nullptr);
        sc.histBuffer = VK_NULL_HANDLE;
    }
    if (sc.histMemory) {
        d.FreeMemory(dev->device, sc.histMemory, nullptr);
        sc.histMemory = VK_NULL_HANDLE;
    }
    if (sc.baseView) {
        d.DestroyImageView(dev->device, sc.baseView, nullptr);
        sc.baseView = VK_NULL_HANDLE;
    }
    if (sc.baseImage) {
        d.DestroyImage(dev->device, sc.baseImage, nullptr);
        sc.baseImage = VK_NULL_HANDLE;
    }
    if (sc.baseMemory) {
        d.FreeMemory(dev->device, sc.baseMemory, nullptr);
        sc.baseMemory = VK_NULL_HANDLE;
    }
    if (sc.maskView) {
        d.DestroyImageView(dev->device, sc.maskView, nullptr);
        sc.maskView = VK_NULL_HANDLE;
    }
    if (sc.maskImage) {
        d.DestroyImage(dev->device, sc.maskImage, nullptr);
        sc.maskImage = VK_NULL_HANDLE;
    }
    if (sc.maskMemory) {
        d.FreeMemory(dev->device, sc.maskMemory, nullptr);
        sc.maskMemory = VK_NULL_HANDLE;
    }

    for (auto &img : sc.images) {
        if (img.fence) {
            d.WaitForFences(dev->device, 1, &img.fence, VK_TRUE, UINT64_MAX);
            d.DestroyFence(dev->device, img.fence, nullptr);
            img.fence = VK_NULL_HANDLE;
        }
        if (img.doneSemaphore) {
            d.DestroySemaphore(dev->device, img.doneSemaphore, nullptr);
            img.doneSemaphore = VK_NULL_HANDLE;
        }
        if (img.cmd && dev->commandPool) {
            d.FreeCommandBuffers(dev->device, dev->commandPool, 1, &img.cmd);
            img.cmd = VK_NULL_HANDLE;
        }
        if (img.srcView) {
            d.DestroyImageView(dev->device, img.srcView, nullptr);
            img.srcView = VK_NULL_HANDLE;
        }
        if (img.srcImage) {
            d.DestroyImage(dev->device, img.srcImage, nullptr);
            img.srcImage = VK_NULL_HANDLE;
        }
        if (img.srcMemory) {
            d.FreeMemory(dev->device, img.srcMemory, nullptr);
            img.srcMemory = VK_NULL_HANDLE;
        }
        if (img.dstView) {
            d.DestroyImageView(dev->device, img.dstView, nullptr);
            img.dstView = VK_NULL_HANDLE;
        }
        if (img.dstImage) {
            d.DestroyImage(dev->device, img.dstImage, nullptr);
            img.dstImage = VK_NULL_HANDLE;
        }
        if (img.dstMemory) {
            d.FreeMemory(dev->device, img.dstMemory, nullptr);
            img.dstMemory = VK_NULL_HANDLE;
        }
        if (img.swapStorageView) {
            d.DestroyImageView(dev->device, img.swapStorageView, nullptr);
            img.swapStorageView = VK_NULL_HANDLE;
        }
        img.tonemapSet = VK_NULL_HANDLE;
        img.histPass1Set = VK_NULL_HANDLE;
        img.histPass2Set = VK_NULL_HANDLE;
        img.uiClusterSet = VK_NULL_HANDLE;
        img.baseBlurSet = VK_NULL_HANDLE;
        img.overlaySet = VK_NULL_HANDLE;
        img.presentSet = VK_NULL_HANDLE;
    }
    sc.images.clear();
    sc.maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.baseLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.active = false;
}

bool createSwapchainResources(DeviceData *dev, SwapchainData &sc)
{
    destroySwapchainResources(dev, sc);
    if (!ensureDeviceResources(dev)) {
        return false;
    }

    auto &d = dev->dispatch;

    if (!createDeviceBuffer(dev, kHistBufferSize,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sc.histBuffer,
                            sc.histMemory)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(SceneStatsGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sc.sceneStatsBuffer,
                          sc.sceneStatsMemory, &sc.sceneStatsMapped)) {
        return false;
    }
    std::memset(sc.sceneStatsMapped, 0, sizeof(SceneStatsGPU));

    sc.halfExtent.width = std::max(1u, sc.extent.width / 2u);
    sc.halfExtent.height = std::max(1u, sc.extent.height / 2u);
    sc.presentRgb10PackMode = rgb10PackModeForFormat(sc.format);
    sc.presentUnormBgra = unormBgraForFormat(sc.format);

    if (!createDeviceImage(dev, sc.halfExtent, kBaseFormat,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, sc.baseImage, sc.baseMemory,
                           sc.baseView)) {
        return false;
    }
    if (!createDeviceImage(dev, sc.extent, kMaskFormat,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, sc.maskImage, sc.maskMemory,
                           sc.maskView)) {
        return false;
    }
    sc.maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.baseLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    uint32_t count = 0;
    d.GetSwapchainImagesKHR(dev->device, sc.swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    d.GetSwapchainImagesKHR(dev->device, sc.swapchain, &count, images.data());
    sc.images.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        auto &res = sc.images[i];
        res.swapImage = images[i];

        if (!createDeviceImage(dev, sc.extent, sc.format,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, res.srcImage,
                               res.srcMemory, res.srcView)) {
            return false;
        }
        if (!createDeviceImage(dev, sc.extent, kComputeDstFormat,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               res.dstImage, res.dstMemory, res.dstView)) {
            return false;
        }

        if (isRgb10SwapFormat(sc.format) || isUnorm8SwapFormat(sc.format)) {
            VkImageViewCreateInfo storageViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            storageViewInfo.image = res.swapImage;
            storageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            storageViewInfo.format = VK_FORMAT_R32_UINT;
            storageViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (d.CreateImageView(dev->device, &storageViewInfo, nullptr, &res.swapStorageView) != VK_SUCCESS) {
                return false;
            }
        }

        const bool hasPresentPipeline = dev->presentPipeline != VK_NULL_HANDLE || dev->presentUnormPipeline != VK_NULL_HANDLE;
        const uint32_t setCount = hasPresentPipeline ? 7u : 6u;
        VkDescriptorSetLayout layouts[7] = {dev->tonemapSetLayout,  dev->histPass1SetLayout,  dev->histPass2SetLayout,
                                            dev->uiClusterSetLayout, dev->baseBlurSetLayout, dev->overlaySetLayout,
                                            dev->presentSetLayout};
        VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAlloc.descriptorPool = dev->descriptorPool;
        dsAlloc.descriptorSetCount = setCount;
        dsAlloc.pSetLayouts = layouts;
        VkDescriptorSet sets[7]{};
        if (d.AllocateDescriptorSets(dev->device, &dsAlloc, sets) != VK_SUCCESS) {
            return false;
        }
        res.tonemapSet = sets[0];
        res.histPass1Set = sets[1];
        res.histPass2Set = sets[2];
        res.uiClusterSet = sets[3];
        res.baseBlurSet = sets[4];
        res.overlaySet = sets[5];
        if (setCount == 7u) {
            res.presentSet = sets[6];
        }

        VkDescriptorImageInfo srcSampleInfo{};
        srcSampleInfo.sampler = dev->sampler;
        srcSampleInfo.imageView = res.srcView;
        srcSampleInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo maskSampleInfo{};
        maskSampleInfo.sampler = dev->sampler;
        maskSampleInfo.imageView = sc.maskView;
        maskSampleInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo toneInfo{};
        toneInfo.buffer = dev->uboBuffer;
        toneInfo.range = sizeof(ToneParamsUBO);

        VkDescriptorBufferInfo lutInfo{};
        lutInfo.buffer = dev->lutBuffer;
        lutInfo.range = sizeof(float) * 4 * 256;

        VkDescriptorImageInfo storageInfo{};
        storageInfo.imageView = res.dstView;
        storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo maskStorageInfo{};
        maskStorageInfo.imageView = sc.maskView;
        maskStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo baseStorageInfo{};
        baseStorageInfo.imageView = sc.baseView;
        baseStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo sceneStatsInfo{};
        sceneStatsInfo.buffer = sc.sceneStatsBuffer;
        sceneStatsInfo.range = sizeof(SceneStatsGPU);

        VkDescriptorBufferInfo histInfo{};
        histInfo.buffer = sc.histBuffer;
        histInfo.range = kHistBufferSize;

        VkDescriptorBufferInfo histParamsInfo{};
        histParamsInfo.buffer = dev->histParamsBuffer;
        histParamsInfo.range = sizeof(HistParamsUBO);

        VkDescriptorBufferInfo adaptParamsInfo{};
        adaptParamsInfo.buffer = dev->adaptParamsBuffer;
        adaptParamsInfo.range = sizeof(AdaptParamsUBO);

        VkDescriptorBufferInfo uiParamsInfo{};
        uiParamsInfo.buffer = dev->uiParamsBuffer;
        uiParamsInfo.range = sizeof(UiParamsUBO);

        VkDescriptorBufferInfo blurParamsInfo{};
        blurParamsInfo.buffer = dev->blurParamsBuffer;
        blurParamsInfo.range = sizeof(BlurParamsUBO);

        VkDescriptorBufferInfo overlayParamsInfo{};
        overlayParamsInfo.buffer = dev->overlayParamsBuffer;
        overlayParamsInfo.range = sizeof(OverlayParamsUBO);

        VkDescriptorBufferInfo presentParamsInfo{};
        presentParamsInfo.buffer = dev->presentParamsBuffer;
        presentParamsInfo.range = sizeof(PresentParamsUBO);

        VkWriteDescriptorSet tonemapWrites[6]{};
        tonemapWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[0].dstSet = res.tonemapSet;
        tonemapWrites[0].dstBinding = 0;
        tonemapWrites[0].descriptorCount = 1;
        tonemapWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tonemapWrites[0].pImageInfo = &srcSampleInfo;
        tonemapWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[1].dstSet = res.tonemapSet;
        tonemapWrites[1].dstBinding = 1;
        tonemapWrites[1].descriptorCount = 1;
        tonemapWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tonemapWrites[1].pBufferInfo = &lutInfo;
        tonemapWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[2].dstSet = res.tonemapSet;
        tonemapWrites[2].dstBinding = 2;
        tonemapWrites[2].descriptorCount = 1;
        tonemapWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tonemapWrites[2].pImageInfo = &maskSampleInfo;
        tonemapWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[3].dstSet = res.tonemapSet;
        tonemapWrites[3].dstBinding = 3;
        tonemapWrites[3].descriptorCount = 1;
        tonemapWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tonemapWrites[3].pBufferInfo = &toneInfo;
        tonemapWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[4].dstSet = res.tonemapSet;
        tonemapWrites[4].dstBinding = 4;
        tonemapWrites[4].descriptorCount = 1;
        tonemapWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tonemapWrites[4].pImageInfo = &storageInfo;
        tonemapWrites[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tonemapWrites[5].dstSet = res.tonemapSet;
        tonemapWrites[5].dstBinding = 5;
        tonemapWrites[5].descriptorCount = 1;
        tonemapWrites[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tonemapWrites[5].pBufferInfo = &sceneStatsInfo;

        VkWriteDescriptorSet histPass1Writes[3]{};
        histPass1Writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass1Writes[0].dstSet = res.histPass1Set;
        histPass1Writes[0].dstBinding = 0;
        histPass1Writes[0].descriptorCount = 1;
        histPass1Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        histPass1Writes[0].pImageInfo = &srcSampleInfo;
        histPass1Writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass1Writes[1].dstSet = res.histPass1Set;
        histPass1Writes[1].dstBinding = 1;
        histPass1Writes[1].descriptorCount = 1;
        histPass1Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        histPass1Writes[1].pBufferInfo = &histInfo;
        histPass1Writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass1Writes[2].dstSet = res.histPass1Set;
        histPass1Writes[2].dstBinding = 2;
        histPass1Writes[2].descriptorCount = 1;
        histPass1Writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        histPass1Writes[2].pBufferInfo = &histParamsInfo;

        VkWriteDescriptorSet histPass2Writes[3]{};
        histPass2Writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass2Writes[0].dstSet = res.histPass2Set;
        histPass2Writes[0].dstBinding = 0;
        histPass2Writes[0].descriptorCount = 1;
        histPass2Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        histPass2Writes[0].pBufferInfo = &histInfo;
        histPass2Writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass2Writes[1].dstSet = res.histPass2Set;
        histPass2Writes[1].dstBinding = 1;
        histPass2Writes[1].descriptorCount = 1;
        histPass2Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        histPass2Writes[1].pBufferInfo = &sceneStatsInfo;
        histPass2Writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        histPass2Writes[2].dstSet = res.histPass2Set;
        histPass2Writes[2].dstBinding = 2;
        histPass2Writes[2].descriptorCount = 1;
        histPass2Writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        histPass2Writes[2].pBufferInfo = &adaptParamsInfo;

        VkWriteDescriptorSet uiClusterWrites[3]{};
        uiClusterWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        uiClusterWrites[0].dstSet = res.uiClusterSet;
        uiClusterWrites[0].dstBinding = 0;
        uiClusterWrites[0].descriptorCount = 1;
        uiClusterWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        uiClusterWrites[0].pImageInfo = &srcSampleInfo;
        uiClusterWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        uiClusterWrites[1].dstSet = res.uiClusterSet;
        uiClusterWrites[1].dstBinding = 1;
        uiClusterWrites[1].descriptorCount = 1;
        uiClusterWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        uiClusterWrites[1].pImageInfo = &maskStorageInfo;
        uiClusterWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        uiClusterWrites[2].dstSet = res.uiClusterSet;
        uiClusterWrites[2].dstBinding = 2;
        uiClusterWrites[2].descriptorCount = 1;
        uiClusterWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uiClusterWrites[2].pBufferInfo = &uiParamsInfo;

        VkWriteDescriptorSet baseBlurWrites[3]{};
        baseBlurWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        baseBlurWrites[0].dstSet = res.baseBlurSet;
        baseBlurWrites[0].dstBinding = 0;
        baseBlurWrites[0].descriptorCount = 1;
        baseBlurWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        baseBlurWrites[0].pImageInfo = &srcSampleInfo;
        baseBlurWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        baseBlurWrites[1].dstSet = res.baseBlurSet;
        baseBlurWrites[1].dstBinding = 1;
        baseBlurWrites[1].descriptorCount = 1;
        baseBlurWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        baseBlurWrites[1].pImageInfo = &baseStorageInfo;
        baseBlurWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        baseBlurWrites[2].dstSet = res.baseBlurSet;
        baseBlurWrites[2].dstBinding = 2;
        baseBlurWrites[2].descriptorCount = 1;
        baseBlurWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        baseBlurWrites[2].pBufferInfo = &blurParamsInfo;

        d.UpdateDescriptorSets(dev->device, 6, tonemapWrites, 0, nullptr);
        d.UpdateDescriptorSets(dev->device, 3, histPass1Writes, 0, nullptr);
        d.UpdateDescriptorSets(dev->device, 3, histPass2Writes, 0, nullptr);
        d.UpdateDescriptorSets(dev->device, 3, uiClusterWrites, 0, nullptr);
        d.UpdateDescriptorSets(dev->device, 3, baseBlurWrites, 0, nullptr);

        VkWriteDescriptorSet overlayWrites[3]{};
        overlayWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        overlayWrites[0].dstSet = res.overlaySet;
        overlayWrites[0].dstBinding = 0;
        overlayWrites[0].descriptorCount = 1;
        overlayWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        overlayWrites[0].pBufferInfo = &overlayParamsInfo;
        overlayWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        overlayWrites[1].dstSet = res.overlaySet;
        overlayWrites[1].dstBinding = 1;
        overlayWrites[1].descriptorCount = 1;
        overlayWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        overlayWrites[1].pImageInfo = &storageInfo;

        VkDescriptorImageInfo fontInfo{};
        uint32_t overlayWriteCount = 2;
        if (dev->overlayFontView != VK_NULL_HANDLE && dev->fontSampler != VK_NULL_HANDLE) {
            fontInfo.sampler = dev->fontSampler;
            fontInfo.imageView = dev->overlayFontView;
            fontInfo.imageLayout = dev->overlayFontLayout;
            overlayWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            overlayWrites[2].dstSet = res.overlaySet;
            overlayWrites[2].dstBinding = 2;
            overlayWrites[2].descriptorCount = 1;
            overlayWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            overlayWrites[2].pImageInfo = &fontInfo;
            overlayWriteCount = 3;
        }
        d.UpdateDescriptorSets(dev->device, overlayWriteCount, overlayWrites, 0, nullptr);

        if (res.presentSet != VK_NULL_HANDLE && res.swapStorageView != VK_NULL_HANDLE) {
            VkDescriptorImageInfo presentInputInfo{};
            presentInputInfo.sampler = dev->sampler;
            presentInputInfo.imageView = res.dstView;
            presentInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo presentOutputInfo{};
            presentOutputInfo.imageView = res.swapStorageView;
            presentOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet presentWrites[3]{};
            presentWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            presentWrites[0].dstSet = res.presentSet;
            presentWrites[0].dstBinding = 0;
            presentWrites[0].descriptorCount = 1;
            presentWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            presentWrites[0].pImageInfo = &presentInputInfo;
            presentWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            presentWrites[1].dstSet = res.presentSet;
            presentWrites[1].dstBinding = 1;
            presentWrites[1].descriptorCount = 1;
            presentWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            presentWrites[1].pImageInfo = &presentOutputInfo;
            presentWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            presentWrites[2].dstSet = res.presentSet;
            presentWrites[2].dstBinding = 2;
            presentWrites[2].descriptorCount = 1;
            presentWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            presentWrites[2].pBufferInfo = &presentParamsInfo;
            d.UpdateDescriptorSets(dev->device, 3, presentWrites, 0, nullptr);
        }

        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = dev->commandPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        if (d.AllocateCommandBuffers(dev->device, &cmdAlloc, &res.cmd) != VK_SUCCESS) {
            return false;
        }

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (d.CreateFence(dev->device, &fenceInfo, nullptr, &res.fence) != VK_SUCCESS) {
            return false;
        }
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d.CreateSemaphore(dev->device, &semInfo, nullptr, &res.doneSemaphore) != VK_SUCCESS) {
            return false;
        }
    }

    sc.active = true;
    logf("swapchain resources created (%u images %ux%u format=%u rgb10Pack=%u unormBgra=%u)", count, sc.extent.width,
         sc.extent.height, static_cast<unsigned>(sc.format), sc.presentRgb10PackMode,
         sc.presentUnormBgra ? 1u : 0u);
    return true;
}

bool processPresent(DeviceData *dev, SwapchainData &sc, uint32_t imageIndex, VkQueue queue, uint32_t waitCount,
                    const VkSemaphore *waitSemaphores, VkSemaphore &outSignal, bool effectOn, bool drawOverlay)
{
    if (!sc.active || imageIndex >= sc.images.size()) {
        return false;
    }

    auto &d = dev->dispatch;
    auto &res = sc.images[imageIndex];
    d.WaitForFences(dev->device, 1, &res.fence, VK_TRUE, UINT64_MAX);

    const AutoHdr::CalibrationSettings settings = effectiveSettings();

    float metaMaxCLL = settings.maxNits;
    float metaMaxFALL = settings.referenceNits;
    float metaEffectivePeak = settings.maxNits;
    if (sc.sceneStatsMapped) {
        const auto *stats = static_cast<const SceneStatsGPU *>(sc.sceneStatsMapped);
        if (stats->initialized != 0) {
            metaMaxCLL = stats->maxCLL;
            metaMaxFALL = stats->maxFALL;
            metaEffectivePeak = stats->effectivePeak;
        }
    }

    d.ResetFences(dev->device, 1, &res.fence);

    float outputMode = 0.0f;
    switch (sc.encoding) {
    case OutputEncoding::LinearScRgb:
        outputMode = 1.0f;
        break;
    case OutputEncoding::SdrPreview:
        outputMode = 2.0f;
        break;
    default:
        outputMode = 0.0f;
        break;
    }

    if (effectOn) {
        uploadToneParams(dev, settings, sc.encoding, sc.inputIsSrgb, sc.extent, sc.halfExtent, sc.format);
    }
    if (drawOverlay) {
        OverlayParamsUBO overlayUbo{};
        const OverlayDrawState hud = overlayDrawState();
        overlayUbo.extentWidth = sc.extent.width;
        overlayUbo.extentHeight = sc.extent.height;
        overlayUbo.intensity = hud.intensity;
        overlayUbo.colorIntensity = hud.colorIntensity;
        overlayUbo.expansionShape = hud.expansionShape;
        overlayUbo.focused = static_cast<float>(hud.focused);
        overlayUbo.outputMode = outputMode;
        overlayUbo.panelNits = hud.panelNits;
        overlayUbo.pointerValid = hud.pointerValid ? 1.0f : 0.0f;
        overlayUbo.pointerX = hud.pointerX;
        overlayUbo.pointerY = hud.pointerY;
        overlayUbo.blackFloor = hud.blackFloor;
        overlayUbo.highlightStretch = hud.highlightStretch;
        uploadOverlayParams(dev, overlayUbo);
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(res.cmd, &begin);

    VkImageMemoryBarrier barriers[3]{};
    for (auto &b : barriers) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }

    // swap: PRESENT -> TRANSFER_SRC
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image = res.swapImage;

    // src: UNDEFINED -> TRANSFER_DST
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = res.srcImage;

    d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, barriers);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {sc.extent.width, sc.extent.height, 1};
    d.CmdCopyImage(res.cmd, res.swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, res.srcImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    if (effectOn) {
        // src: TRANSFER_DST -> SHADER_READ
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = res.srcImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, barriers);

        d.CmdFillBuffer(res.cmd, sc.histBuffer, 0, kHistBufferSize, 0);

        VkBufferMemoryBarrier histBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        histBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        histBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        histBarrier.buffer = sc.histBuffer;
        histBarrier.offset = 0;
        histBarrier.size = kHistBufferSize;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                             &histBarrier, 0, nullptr);

        const uint32_t gx = (sc.extent.width + 15u) / 16u;
        const uint32_t gy = (sc.extent.height + 15u) / 16u;
        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->histPass1Pipeline);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->histPass1PipelineLayout, 0, 1,
                                &res.histPass1Set, 0, nullptr);
        d.CmdDispatch(res.cmd, gx, gy, 1);

        histBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        histBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &histBarrier, 0, nullptr);

        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->histPass2Pipeline);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->histPass2PipelineLayout, 0, 1,
                                &res.histPass2Set, 0, nullptr);
        d.CmdDispatch(res.cmd, 1, 1, 1);

        VkBufferMemoryBarrier sceneStatsBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        sceneStatsBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sceneStatsBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sceneStatsBarrier.buffer = sc.sceneStatsBuffer;
        sceneStatsBarrier.offset = 0;
        sceneStatsBarrier.size = sizeof(SceneStatsGPU);
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &sceneStatsBarrier, 0, nullptr);

        const bool maskWasUndefined = (sc.maskLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        const bool baseWasUndefined = (sc.baseLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        barriers[0].srcAccessMask = maskWasUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].oldLayout = sc.maskLayout;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].image = sc.maskImage;

        barriers[1].srcAccessMask = baseWasUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[1].oldLayout = sc.baseLayout;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].image = sc.baseImage;
        d.CmdPipelineBarrier(res.cmd,
                             maskWasUndefined && baseWasUndefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                  : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        sc.maskLayout = VK_IMAGE_LAYOUT_GENERAL;
        sc.baseLayout = VK_IMAGE_LAYOUT_GENERAL;

        // UI detection temporarily disabled — flip to true to re-enable dispatch.
        constexpr bool kEnableUiDetection = false;
        if (kEnableUiDetection) {
            const uint32_t uiGx = (sc.extent.width + 7u) / 8u;
            const uint32_t uiGy = (sc.extent.height + 7u) / 8u;
            d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->uiClusterPipeline);
            d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->uiClusterPipelineLayout, 0, 1,
                                    &res.uiClusterSet, 0, nullptr);
            d.CmdDispatch(res.cmd, uiGx, uiGy, 1);
        }

        const uint32_t blurGx = (sc.halfExtent.width + 15u) / 16u;
        const uint32_t blurGy = (sc.halfExtent.height + 15u) / 16u;
        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->baseBlurPipeline);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->baseBlurPipelineLayout, 0, 1,
                                &res.baseBlurSet, 0, nullptr);
        d.CmdDispatch(res.cmd, blurGx, blurGy, 1);

        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = sc.maskImage;

        barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].image = sc.baseImage;

        barriers[2].srcAccessMask = 0;
        barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].image = res.dstImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 3, barriers);
        sc.maskLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sc.baseLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->tonemapPipeline);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->tonemapPipelineLayout, 0, 1, &res.tonemapSet,
                                0, nullptr);
        d.CmdDispatch(res.cmd, gx, gy, 1);
    } else {
        // Identity path: blit swapchain copy into compute dst for optional overlay + present.
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = res.srcImage;

        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = res.dstImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, barriers);

        VkImageBlit idBlit{};
        idBlit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        idBlit.dstSubresource = idBlit.srcSubresource;
        idBlit.srcOffsets[1] = {static_cast<int32_t>(sc.extent.width), static_cast<int32_t>(sc.extent.height), 1};
        idBlit.dstOffsets[1] = idBlit.srcOffsets[1];
        d.CmdBlitImage(res.cmd, res.srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, res.dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &idBlit, VK_FILTER_NEAREST);

        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = drawOverlay ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                                : VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = drawOverlay ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = res.dstImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             drawOverlay ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, barriers);
    }

    if (drawOverlay) {
        if (effectOn) {
            // dst still GENERAL after tonemap write
            barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[0].image = res.dstImage;
            d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, barriers);
        }

        const uint32_t gx = (sc.extent.width + 15u) / 16u;
        const uint32_t gy = (sc.extent.height + 15u) / 16u;
        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->overlayPipeline);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->overlayPipelineLayout, 0, 1,
                                &res.overlaySet, 0, nullptr);
        d.CmdDispatch(res.cmd, gx, gy, 1);

        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = res.dstImage;

        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, barriers);
    } else if (effectOn) {
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = res.dstImage;

        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, barriers);
    } else {
        // Identity without overlay: dst already TRANSFER_SRC
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, barriers);
    }

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {static_cast<int32_t>(sc.extent.width), static_cast<int32_t>(sc.extent.height), 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];

    const bool usePresentRgb10 =
        isRgb10SwapFormat(sc.format) && dev->presentPipeline != VK_NULL_HANDLE && res.presentSet != VK_NULL_HANDLE
        && res.swapStorageView != VK_NULL_HANDLE;
    const bool usePresentUnorm =
        isUnorm8SwapFormat(sc.format) && dev->presentUnormPipeline != VK_NULL_HANDLE && res.presentSet != VK_NULL_HANDLE
        && res.swapStorageView != VK_NULL_HANDLE;

    if (usePresentRgb10 || usePresentUnorm) {
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = res.dstImage;

        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, barriers);

        if (dev->presentParamsMapped) {
            PresentParamsUBO presentParams{};
            presentParams.extentWidth = sc.extent.width;
            presentParams.extentHeight = sc.extent.height;
            presentParams.rgb10PackMode = sc.presentRgb10PackMode;
            presentParams.unormBgra = sc.presentUnormBgra ? 1u : 0u;
            std::memcpy(dev->presentParamsMapped, &presentParams, sizeof(presentParams));
        }

        const uint32_t gx = (sc.extent.width + 15u) / 16u;
        const uint32_t gy = (sc.extent.height + 15u) / 16u;
        const VkPipeline presentPipe = usePresentRgb10 ? dev->presentPipeline : dev->presentUnormPipeline;
        d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, presentPipe);
        d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->presentPipelineLayout, 0, 1, &res.presentSet,
                                0, nullptr);
        d.CmdDispatch(res.cmd, gx, gy, 1);

        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = 0;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, barriers);
    } else {
        d.CmdBlitImage(res.cmd, res.dstImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, res.swapImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = 0;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].image = res.swapImage;
        d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, barriers);
    }

    d.EndCommandBuffer(res.cmd);

    std::vector<VkPipelineStageFlags> waitStages(waitCount, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &res.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &res.doneSemaphore;
    VkQueue submitQueue = dev->graphicsQueue != VK_NULL_HANDLE ? dev->graphicsQueue : queue;
    if (d.QueueSubmit(submitQueue, 1, &submit, res.fence) != VK_SUCCESS) {
        return false;
    }

    outSignal = res.doneSemaphore;

    if (effectOn && wantHdrMetadata() && dev->hdrMetadataExt && d.SetHdrMetadataEXT && sc.hdrColorspace) {
        VkHdrMetadataEXT meta{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        meta.displayPrimaryRed = {0.708f, 0.292f};
        meta.displayPrimaryGreen = {0.170f, 0.797f};
        meta.displayPrimaryBlue = {0.131f, 0.046f};
        meta.whitePoint = {0.3127f, 0.3290f};
        meta.maxLuminance = metaEffectivePeak;
        meta.minLuminance = 0.0f;
        meta.maxContentLightLevel = metaMaxCLL;
        meta.maxFrameAverageLightLevel = metaMaxFALL;
        d.SetHdrMetadataEXT(dev->device, 1, &sc.swapchain, &meta);
    }

    return true;
}

} // namespace AutoHdrVk
