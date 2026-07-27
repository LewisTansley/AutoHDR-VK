#include "layer_common.hpp"

#include "calibration.hpp"
#include "tone_curve.hpp"

#include "tonemap.vert.spv.h"
#include "tonemap.frag.spv.h"

#include <algorithm>

namespace AutoHdrVk {

namespace {

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
                      bool inputIsSrgb)
{
    if (!dev->uboMapped || !dev->lutMapped) {
        return;
    }

    AutoHdr::ToneCurveEndpoints endpoints;
    endpoints.peakNits = settings.maxNits;
    endpoints.visualReferenceNits = settings.referenceNits;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint.x > 0.0f ? settings.sdrMaxPoint
                                                          : AutoHdr::Vec2{settings.referenceNits, settings.maxNits};

    const auto full = AutoHdr::buildFullCurve(endpoints, settings.toneCurvePoints);
    const float span = std::max(endpoints.sdrMaxPoint.x, settings.referenceNits);
    float lut[AutoHdr::kToneCurveLutSize];
    AutoHdr::buildToneCurveLut(full, span, lut, AutoHdr::kToneCurveLutSize);
    float packed[256 * 4];
    for (int i = 0; i < AutoHdr::kToneCurveLutSize; ++i) {
        packed[i] = lut[i];
    }
    std::memcpy(dev->lutMapped, packed, sizeof(packed));

    ToneParamsUBO ubo{};
    ubo.blackPoint = settings.blackPoint;
    ubo.colorIntensity = settings.colorIntensity;
    ubo.gamutExpansion = settings.gamutExpansion;
    ubo.referenceNits = settings.referenceNits;
    ubo.peakNits = settings.maxNits;
    ubo.toneCurveInputSpan = span;
    ubo.highlightSoftness = settings.highlightSoftness;
    ubo.perceptualColorEnabled = settings.perceptualColor ? 1.0f : 0.0f;
    ubo.inputIsSrgb = inputIsSrgb ? 1.0f : 0.0f;
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
    std::memcpy(dev->uboMapped, &ubo, sizeof(ubo));
}

void destroyDeviceResources(DeviceData *dev)
{
    if (!dev) {
        return;
    }
    auto &d = dev->dispatch;
    if (dev->pipeline) {
        d.DestroyPipeline(dev->device, dev->pipeline, nullptr);
        dev->pipeline = VK_NULL_HANDLE;
    }
    if (dev->renderPass) {
        d.DestroyRenderPass(dev->device, dev->renderPass, nullptr);
        dev->renderPass = VK_NULL_HANDLE;
    }
    if (dev->pipelineLayout) {
        d.DestroyPipelineLayout(dev->device, dev->pipelineLayout, nullptr);
        dev->pipelineLayout = VK_NULL_HANDLE;
    }
    if (dev->setLayout) {
        d.DestroyDescriptorSetLayout(dev->device, dev->setLayout, nullptr);
        dev->setLayout = VK_NULL_HANDLE;
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
    if (dev->commandPool) {
        d.DestroyCommandPool(dev->device, dev->commandPool, nullptr);
        dev->commandPool = VK_NULL_HANDLE;
    }
    dev->pipelineReady = false;
    dev->renderPassFormat = VK_FORMAT_UNDEFINED;
}

bool ensureDeviceResources(DeviceData *dev, VkFormat swapFormat)
{
    if (dev->pipelineReady && dev->renderPassFormat == swapFormat) {
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

    if (!createHostBuffer(dev, sizeof(float) * 4 * 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->lutBuffer,
                          dev->lutMemory, &dev->lutMapped)) {
        return false;
    }
    if (!createHostBuffer(dev, sizeof(ToneParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, dev->uboBuffer,
                          dev->uboMemory, &dev->uboMapped)) {
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 3;
    setLayoutInfo.pBindings = bindings;
    if (d.CreateDescriptorSetLayout(dev->device, &setLayoutInfo, nullptr, &dev->setLayout) != VK_SUCCESS) {
        return false;
    }

    VkPipelineLayoutCreateInfo pipeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &dev->setLayout;
    if (d.CreatePipelineLayout(dev->device, &pipeLayoutInfo, nullptr, &dev->pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkAttachmentDescription color{};
    color.format = swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &color;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (d.CreateRenderPass(dev->device, &rpInfo, nullptr, &dev->renderPass) != VK_SUCCESS) {
        return false;
    }
    dev->renderPassFormat = swapFormat;

    VkShaderModule vert = createShaderModule(dev, tonemap_vert_spv, sizeof(tonemap_vert_spv));
    VkShaderModule frag = createShaderModule(dev, tonemap_frag_spv, sizeof(tonemap_frag_spv));
    if (!vert || !frag) {
        if (vert) {
            d.DestroyShaderModule(dev->device, vert, nullptr);
        }
        if (frag) {
            d.DestroyShaderModule(dev->device, frag, nullptr);
        }
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    VkRect2D scissor{};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dyn.pDynamicStates = dynStates.data();

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = dev->pipelineLayout;
    gp.renderPass = dev->renderPass;
    gp.subpass = 0;

    const VkResult pipeResult = d.CreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &gp, nullptr, &dev->pipeline);
    d.DestroyShaderModule(dev->device, vert, nullptr);
    d.DestroyShaderModule(dev->device, frag, nullptr);
    if (pipeResult != VK_SUCCESS) {
        return false;
    }

    // Large pool: up to 16 swapchain images typically.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 64;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 128;
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 64;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (d.CreateDescriptorPool(dev->device, &dp, nullptr, &dev->descriptorPool) != VK_SUCCESS) {
        return false;
    }

    d.GetDeviceQueue(dev->device, dev->graphicsQueueFamily, 0, &dev->graphicsQueue);
    dev->pipelineReady = true;
    logf("device pipeline ready (format=%u)", static_cast<unsigned>(swapFormat));
    return true;
}

void destroySwapchainResources(DeviceData *dev, SwapchainData &sc)
{
    auto &d = dev->dispatch;
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
        if (img.framebuffer) {
            d.DestroyFramebuffer(dev->device, img.framebuffer, nullptr);
            img.framebuffer = VK_NULL_HANDLE;
        }
        if (img.swapView) {
            d.DestroyImageView(dev->device, img.swapView, nullptr);
            img.swapView = VK_NULL_HANDLE;
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
    }
    sc.images.clear();
    sc.active = false;
}

bool createSwapchainResources(DeviceData *dev, SwapchainData &sc)
{
    destroySwapchainResources(dev, sc);
    if (!ensureDeviceResources(dev, sc.format)) {
        return false;
    }

    auto &d = dev->dispatch;
    uint32_t count = 0;
    d.GetSwapchainImagesKHR(dev->device, sc.swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    d.GetSwapchainImagesKHR(dev->device, sc.swapchain, &count, images.data());
    sc.images.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        auto &res = sc.images[i];
        res.swapImage = images[i];

        VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = sc.format;
        imgInfo.extent = {sc.extent.width, sc.extent.height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (d.CreateImage(dev->device, &imgInfo, nullptr, &res.srcImage) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req{};
        d.GetImageMemoryRequirements(dev->device, res.srcImage, &req);
        const uint32_t type = findMemoryType(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (d.AllocateMemory(dev->device, &alloc, nullptr, &res.srcMemory) != VK_SUCCESS) {
            return false;
        }
        d.BindImageMemory(dev->device, res.srcImage, res.srcMemory, 0);

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = sc.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        viewInfo.image = res.srcImage;
        if (d.CreateImageView(dev->device, &viewInfo, nullptr, &res.srcView) != VK_SUCCESS) {
            return false;
        }
        viewInfo.image = res.swapImage;
        if (d.CreateImageView(dev->device, &viewInfo, nullptr, &res.swapView) != VK_SUCCESS) {
            return false;
        }

        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = dev->renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &res.swapView;
        fb.width = sc.extent.width;
        fb.height = sc.extent.height;
        fb.layers = 1;
        if (d.CreateFramebuffer(dev->device, &fb, nullptr, &res.framebuffer) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAlloc.descriptorPool = dev->descriptorPool;
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts = &dev->setLayout;
        if (d.AllocateDescriptorSets(dev->device, &dsAlloc, &res.descriptorSet) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = dev->sampler;
        imageInfo.imageView = res.srcView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo lutInfo{};
        lutInfo.buffer = dev->lutBuffer;
        lutInfo.range = sizeof(float) * 4 * 256;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = dev->uboBuffer;
        uboInfo.range = sizeof(ToneParamsUBO);

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = res.descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = res.descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &lutInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = res.descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &uboInfo;
        d.UpdateDescriptorSets(dev->device, 3, writes, 0, nullptr);

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
    logf("swapchain resources created (%u images %ux%u)", count, sc.extent.width, sc.extent.height);
    return true;
}

bool processPresent(DeviceData *dev, SwapchainData &sc, uint32_t imageIndex, VkQueue queue, uint32_t waitCount,
                    const VkSemaphore *waitSemaphores, VkSemaphore &outSignal)
{
    if (!sc.active || imageIndex >= sc.images.size()) {
        return false;
    }

    auto &d = dev->dispatch;
    auto &res = sc.images[imageIndex];
    d.WaitForFences(dev->device, 1, &res.fence, VK_TRUE, UINT64_MAX);
    d.ResetFences(dev->device, 1, &res.fence);

    uploadToneParams(dev, activeSettings(), sc.encoding, sc.inputIsSrgb);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(res.cmd, &begin);

    // swapchain: PRESENT -> TRANSFER_SRC
    // src: UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = res.swapImage;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1] = barriers[0];
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

    // src: TRANSFER_DST -> SHADER_READ
    // swap: TRANSFER_SRC -> COLOR_ATTACHMENT
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = res.srcImage;

    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].image = res.swapImage;

    d.CmdPipelineBarrier(res.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, barriers);

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = dev->renderPass;
    rpBegin.framebuffer = res.framebuffer;
    rpBegin.renderArea.extent = sc.extent;
    d.CmdBeginRenderPass(res.cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    d.CmdBindPipeline(res.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(sc.extent.width);
    viewport.height = static_cast<float>(sc.extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = sc.extent;

    // Dynamic viewport/scissor — need CmdSetViewport/CmdSetScissor. Add to dispatch.
    // Fallback: recreate pipeline with static viewport if missing — add function pointers.
    auto setViewport = reinterpret_cast<PFN_vkCmdSetViewport>(
        d.GetDeviceProcAddr(dev->device, "vkCmdSetViewport"));
    auto setScissor = reinterpret_cast<PFN_vkCmdSetScissor>(d.GetDeviceProcAddr(dev->device, "vkCmdSetScissor"));
    if (setViewport) {
        setViewport(res.cmd, 0, 1, &viewport);
    }
    if (setScissor) {
        setScissor(res.cmd, 0, 1, &scissor);
    }

    d.CmdBindDescriptorSets(res.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->pipelineLayout, 0, 1, &res.descriptorSet, 0,
                            nullptr);
    d.CmdDraw(res.cmd, 3, 1, 0, 0);
    d.CmdEndRenderPass(res.cmd);
    d.EndCommandBuffer(res.cmd);

    std::vector<VkPipelineStageFlags> waitStages(waitCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &res.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &res.doneSemaphore;
    if (d.QueueSubmit(queue, 1, &submit, res.fence) != VK_SUCCESS) {
        return false;
    }

    outSignal = res.doneSemaphore;

    if (wantHdrMetadata() && dev->hdrMetadataExt && d.SetHdrMetadataEXT && sc.hdrColorspace) {
        const auto settings = activeSettings();
        VkHdrMetadataEXT meta{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        meta.displayPrimaryRed = {0.708f, 0.292f};
        meta.displayPrimaryGreen = {0.170f, 0.797f};
        meta.displayPrimaryBlue = {0.131f, 0.046f};
        meta.whitePoint = {0.3127f, 0.3290f};
        meta.maxLuminance = settings.maxNits;
        meta.minLuminance = 0.0f;
        meta.maxContentLightLevel = settings.maxNits;
        meta.maxFrameAverageLightLevel = settings.referenceNits;
        d.SetHdrMetadataEXT(dev->device, 1, &sc.swapchain, &meta);
    }

    return true;
}

} // namespace AutoHdrVk
