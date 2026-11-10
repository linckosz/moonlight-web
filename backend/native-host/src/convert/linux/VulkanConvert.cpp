/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "VulkanConvert.h"

#include "../../core/Log.h"
#include "../../encode/linux/VulkanHevcEncoder.h"
#include "../../platform/linux/vulkan/VulkanDevice.h"
#include "../../platform/macos/FrameFit.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// glslangValidator's output (CMakeLists.txt): shaders/vk_scale.comp built once
// per direction, and shaders/vk_nv12.comp. They name uint32_t without including
// anything, hence last.
#include "VkNv12Spv.h"
#include "VkScaleHSpv.h"
#include "VkScaleVSpv.h"

// Exporting a DMA-BUF's implicit fences (Linux 6.0). Ubuntu 22.04's headers
// predate it; the running kernel is what counts, and one without it refuses.
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file
{
    __u32 flags;
    __s32 fd;
};
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#endif

namespace mw::native::convert {
namespace {

using vulkan::resultText;

/// The Vulkan format a scanout fourcc samples as. The same four families
/// GlConvert takes: 8 bits a channel, and the 10-bit SDR desktop KWin scans out.
VkFormat sourceFormat(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

/// The scanout buffer a conversion reads, what was checked of its format last:
/// the driver's answer does not change from one frame to the next.
struct SourceFormat
{
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int planes = 0;
    bool usable = false;
};

struct ScalePush
{
    int32_t origin[2];
    int32_t size[2];
    int32_t extent[2];
    /// Where the picture starts in the input (KmsFrame::sourceX): the
    /// horizontal pass only; the intermediate is the picture alone.
    int32_t offset[2];
    float len;
    float fixedLen;
    float dilate;
    int32_t taps;
    /// How the input holds the picture (KmsFrame::orientation): quarter
    /// turns, then 1 when mirrored. The horizontal pass only.
    int32_t orient[2];
};
static_assert(sizeof(ScalePush) == 56, "vk_scale.comp's push constants");

struct Nv12Push
{
    float cursorRect[4];
    /// The picture inside the sampled image, in its UV: (0,0,1,1) but for a
    /// screen's window of an X11 root (GlConvert's SourceRect).
    float sourceRect[4];
    int32_t size[2];
    float cursorEnabled;
    float pad;
    /// How the sampled image holds the picture: as ScalePush's, (0,0) for the
    /// resampled picture, upright already.
    int32_t orient[2];
};
static_assert(sizeof(Nv12Push) == 56, "vk_nv12.comp's push constants");

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                   uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                                   uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
{
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

} // namespace

struct VulkanConvert::Impl
{
    /// Shared with the Vulkan Video encoder on that route.
    std::shared_ptr<vulkan::VulkanDevice> device;
    const vulkan::DeviceFunctions* fn = nullptr;
    VkDevice dev = VK_NULL_HANDLE;

    VkSampler linear = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout scaleLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout nv12Layout = VK_NULL_HANDLE;
    VkPipelineLayout scalePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout nv12PipelineLayout = VK_NULL_HANDLE;
    VkPipeline scaleH = VK_NULL_HANDLE;
    VkPipeline scaleV = VK_NULL_HANDLE;
    VkPipeline nv12 = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkDescriptorSet scaleHSet = VK_NULL_HANDLE;
    VkDescriptorSet scaleVSet = VK_NULL_HANDLE;
    VkDescriptorSet nv12Set = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    /// Binary, for the source's implicit fence imported as a sync_file.
    VkSemaphore implicitFence = VK_NULL_HANDLE;

    // The resample pass (Lanczos2): horizontal into mid (picture-wide,
    // source-high, RGBA16F linear), vertical into scaled (output-sized RGBA8,
    // sRGB-encoded) — which the conversion then samples where it would the
    // scanout. GlConvert's two FBOs.
    Image mid;
    Image scaled;

    Image cursorPixels; // B8G8R8A8: the CursorState bytes as they are
    Image cursorInvert; // R8
    bool haveCursor = false;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkDeviceSize stagingSize = 0;

    // The encoder's surface, its two planes: VA-API's, imported.
    Image luma;
    Image chroma;
    // Or the Vulkan Video encoder's input, an image of this device: its
    // plane views are the encoder's, written here, never owned.
    VkImage encoderInput = VK_NULL_HANDLE;
    bool targetBound = false;
    bool targetTouched = false;

    SourceFormat sourceFormat;
    uint64_t frames = 0;
    bool failed = false;
    bool saidNoFence = false;

    // The portal's shared memory (C13.10): a picture of the source's own
    // format that each conversion first fills — by the GPU, from the mapping
    // itself (VK_EXT_external_memory_host), or from a copy the CPU made into
    // a buffer of ours, where the driver refuses that mapping.
    Image hostPicture;
    VkFormat hostPictureFormat = VK_FORMAT_UNDEFINED;
    VkBuffer hostCopy = VK_NULL_HANDLE;
    VkDeviceMemory hostCopyMemory = VK_NULL_HANDLE;
    void* hostCopyMapped = nullptr;
    VkDeviceSize hostCopySize = 0;
    /// The mapping could not be imported once: the CPU copies from then on.
    bool hostImportRefused = false;
    bool saidHostPath = false;
    /// MW_VK_CONVERT_FAIL_AT=N: the Nth conversion fails as a lost device
    /// would, and 0 fails init() as a machine without Vulkan would — how the
    /// session's fallback to GL is tested, and benched. (Not VK_DRIVER_FILES:
    /// the loader reads it with secure_getenv, which a test binary carrying
    /// file capabilities never sees.) Never set by the product.
    uint64_t failAt = 0;
};

VulkanConvert::VulkanConvert()
    : d(std::make_unique<Impl>())
{}

VulkanConvert::~VulkanConvert()
{
    stop();
}

namespace {

/// An image of our own, device-local, optimal tiling.
VkResult createImage(vulkan::VulkanDevice& device, VkFormat format, int width, int height,
                     VkImageUsageFlags usage, Image& out)
{
    const vulkan::DeviceFunctions& fn = device.fn();
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = fn.vkCreateImage(device.device(), &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req = {};
    fn.vkGetImageMemoryRequirements(device.device(), out.image, &req);
    const uint32_t type =
        device.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type != UINT32_MAX ? type : device.memoryType(req.memoryTypeBits, 0);
    if (mai.memoryTypeIndex == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    r = fn.vkAllocateMemory(device.device(), &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) return r;
    r = fn.vkBindImageMemory(device.device(), out.image, out.memory, 0);
    if (r != VK_SUCCESS) return r;
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = fn.vkCreateImageView(device.device(), &vci, nullptr, &out.view);
    out.width = width;
    out.height = height;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return r;
}

/// A DMA-BUF as an image of @p format at @p modifier, its planes where the
/// buffer has them, one memory import of its one object.
VkResult importImage(vulkan::VulkanDevice& device, int fd, uint64_t modifier, int planes,
                     const uint32_t* offsets, const uint32_t* pitches, VkFormat format, int width,
                     int height, VkImageUsageFlags usage, Image& out)
{
    const vulkan::DeviceFunctions& fn = device.fn();
    VkSubresourceLayout layouts[4] = {};
    for (int i = 0; i < planes && i < 4; ++i) {
        layouts[i].offset = offsets[i];
        layouts[i].rowPitch = pitches[i];
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitInfo = {};
    explicitInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    explicitInfo.drmFormatModifier = modifier;
    explicitInfo.drmFormatModifierPlaneCount = static_cast<uint32_t>(planes);
    explicitInfo.pPlaneLayouts = layouts;
    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &explicitInfo;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = fn.vkCreateImage(device.device(), &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req = {};
    fn.vkGetImageMemoryRequirements(device.device(), out.image, &req);
    VkMemoryFdPropertiesKHR fdProps = {};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    r = fn.vkGetMemoryFdPropertiesKHR(device.device(),
                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps);
    if (r != VK_SUCCESS) return r;
    const uint32_t type = device.memoryType(req.memoryTypeBits & fdProps.memoryTypeBits, 0);
    if (type == UINT32_MAX) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // Vulkan takes ownership of the fd it imports: it gets a copy, the frame
    // keeps its own.
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = ::dup(fd);
    if (import.fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.image = out.image;
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = fn.vkAllocateMemory(device.device(), &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        ::close(import.fd);
        return r;
    }
    r = fn.vkBindImageMemory(device.device(), out.image, out.memory, 0);
    if (r != VK_SUCCESS) return r;
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = fn.vkCreateImageView(device.device(), &vci, nullptr, &out.view);
    out.width = width;
    out.height = height;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return r;
}

/// Memory this process mapped — the portal's shared memory — as a buffer the
/// GPU copies from (VK_EXT_external_memory_host): from @p address down to the
/// alignment the driver wants, @p size bytes past @p address, rounded up.
/// @p offset says where @p address lands in the buffer.
VkResult importHostMemory(vulkan::VulkanDevice& device, const void* address, size_t size,
                          VkBuffer& buffer, VkDeviceMemory& memory, VkDeviceSize& offset)
{
    const vulkan::DeviceFunctions& fn = device.fn();
    const VkDeviceSize align = std::max<VkDeviceSize>(device.hostPointerAlignment(), 1);
    const auto start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t base = start / align * align;
    offset = start - base;
    const VkDeviceSize bytes = (offset + size + align - 1) / align * align;
    // The driver reads it, never writes: the pointer is only const for us.
    void* pointer = reinterpret_cast<void*>(base);
    VkMemoryHostPointerPropertiesEXT props = {};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    VkResult r = fn.vkGetMemoryHostPointerPropertiesEXT(
        device.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &props);
    if (r != VK_SUCCESS) return r;
    VkExternalMemoryBufferCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &external;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = fn.vkCreateBuffer(device.device(), &bci, nullptr, &buffer);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req = {};
    fn.vkGetBufferMemoryRequirements(device.device(), buffer, &req);
    const uint32_t type = device.memoryType(req.memoryTypeBits & props.memoryTypeBits, 0);
    if (type == UINT32_MAX) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImportMemoryHostPointerInfoEXT import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = pointer;
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &import;
    mai.allocationSize = bytes;
    mai.memoryTypeIndex = type;
    r = fn.vkAllocateMemory(device.device(), &mai, nullptr, &memory);
    if (r != VK_SUCCESS) return r;
    return fn.vkBindBufferMemory(device.device(), buffer, memory, 0);
}

void destroyImage(vulkan::VulkanDevice& device, Image& image)
{
    const vulkan::DeviceFunctions& fn = device.fn();
    if (image.view) fn.vkDestroyImageView(device.device(), image.view, nullptr);
    if (image.image) fn.vkDestroyImage(device.device(), image.image, nullptr);
    if (image.memory) fn.vkFreeMemory(device.device(), image.memory, nullptr);
    image = Image{};
}

VkShaderModule shaderModule(const vulkan::DeviceFunctions& fn, VkDevice device,
                            const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo smi = {};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = bytes;
    smi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    return fn.vkCreateShaderModule(device, &smi, nullptr, &module) == VK_SUCCESS ? module
                                                                                 : VK_NULL_HANDLE;
}

} // namespace

bool VulkanConvert::init(const std::string& renderNode, uint32_t sourceFourcc, int sourceWidth,
                         int sourceHeight, int outputWidth, int outputHeight, ScaleFilter filter,
                         std::string& error)
{
    if (!setUp(sourceFourcc, sourceWidth, sourceHeight, outputWidth, outputHeight, filter, error))
        return false;
    d->device = vulkan::VulkanDevice::open(renderNode, m_WantHigh, error);
    if (!d->device) return false;
    return start(error);
}

bool VulkanConvert::init(const std::shared_ptr<vulkan::VulkanDevice>& device, uint32_t sourceFourcc,
                         int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                         ScaleFilter filter, std::string& error)
{
    if (!setUp(sourceFourcc, sourceWidth, sourceHeight, outputWidth, outputHeight, filter, error))
        return false;
    if (!device) {
        error = "no Vulkan device to convert on";
        return false;
    }
    d->device = device;
    return start(error);
}

bool VulkanConvert::setUp(uint32_t sourceFourcc, int sourceWidth, int sourceHeight, int outputWidth,
                          int outputHeight, ScaleFilter filter, std::string& error)
{
    stop();
    d = std::make_unique<Impl>();

    // The formats GlConvert takes, for the same reason: an imported image is
    // sampled normalised, whatever its depth. HDR is not written here either.
    if (sourceFormat(sourceFourcc) == VK_FORMAT_UNDEFINED) {
        error = "no Vulkan conversion for scanout format " + std::to_string(sourceFourcc);
        return false;
    }
    m_SourceFourcc = sourceFourcc;
    m_ResampleCost.reset();
    m_ResampleCostTaken = false;
    m_CursorShapeVersion = 0;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    // GlConvert's geometry, to the pixel: the resample pass only where there
    // is something to resample, a source of another shape fitted between bars.
    const bool scaling = m_OutputWidth != m_SourceWidth || m_OutputHeight != m_SourceHeight;
    m_Filter = scaling ? filter : ScaleFilter::Bilinear;
    m_Letterboxed = false;
    m_PictureX = m_PictureY = 0;
    m_PictureWidth = m_OutputWidth;
    m_PictureHeight = m_OutputHeight;
    if (m_Filter != ScaleFilter::Bilinear) {
        const platform::FrameFit fit =
            platform::frameFit(m_SourceWidth, m_SourceHeight, m_OutputWidth, m_OutputHeight);
        const int w = static_cast<int>(std::lround(m_SourceWidth * fit.scale));
        const int h = static_cast<int>(std::lround(m_SourceHeight * fit.scale));
        if (w < m_OutputWidth - 1 || h < m_OutputHeight - 1) {
            m_Letterboxed = true;
            m_PictureWidth = std::max(2, w);
            m_PictureHeight = std::max(2, h);
            m_PictureX = (m_OutputWidth - m_PictureWidth) / 2;
            m_PictureY = (m_OutputHeight - m_PictureHeight) / 2;
        }
    }

    if (const char* failAt = std::getenv("MW_VK_CONVERT_FAIL_AT"); failAt && *failAt) {
        d->failAt = std::strtoull(failAt, nullptr, 10);
        if (!d->failAt) {
            error = "fault injected (MW_VK_CONVERT_FAIL_AT=0): no Vulkan here";
            return false;
        }
        log::info("[native] MW_VK_CONVERT_FAIL_AT in effect: conversion " +
                  std::to_string(d->failAt) + " fails as a lost device would");
    }
    return true;
}

bool VulkanConvert::start(std::string& error)
{
    d->fn = &d->device->fn();
    d->dev = d->device->device();
    m_Priority = d->device->queueDescription();
    log::info("[native] Vulkan conversion on " + d->device->name() + ", " + m_Priority);

    if (!createPipelines(error)) return false;
    if (m_Filter != ScaleFilter::Bilinear && !createScaler(error)) return false;

    const bool scaling = m_OutputWidth != m_SourceWidth || m_OutputHeight != m_SourceHeight;
    log::info("[native] colour conversion: " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) + " XRGB -> " + std::to_string(m_OutputWidth) + "x" +
              std::to_string(m_OutputHeight) + " NV12 4:2:0 (BT.709 limited), via Vulkan compute" +
              (!scaling                            ? ", 1:1"
               : m_Filter == ScaleFilter::Bilinear ? ", scaled bilinear in the pass"
                                                   : ", scaled Lanczos-2 (linear light)") +
              (m_Letterboxed ? ", letterboxed to " + std::to_string(m_PictureWidth) + "x" +
                                   std::to_string(m_PictureHeight)
                             : ""));
    return true;
}

bool VulkanConvert::createPipelines(std::string& error)
{
    const vulkan::DeviceFunctions& fn = *d->fn;
    VkDevice dev = d->dev;

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (fn.vkCreateSampler(dev, &sci, nullptr, &d->linear) != VK_SUCCESS) {
        error = "Vulkan: no sampler";
        return false;
    }
    // Point sampling for the invert mask — a half-inverted pixel is not a
    // thing — and for the inputs the resample reads with texelFetch.
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    if (fn.vkCreateSampler(dev, &sci, nullptr, &d->nearest) != VK_SUCCESS) {
        error = "Vulkan: no sampler";
        return false;
    }

    auto setLayout = [&](const VkDescriptorType* types, uint32_t count,
                         VkDescriptorSetLayout& out) {
        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (uint32_t i = 0; i < count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dsl = {};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = count;
        dsl.pBindings = bindings;
        return fn.vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &out) == VK_SUCCESS;
    };
    const VkDescriptorType scaleTypes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    const VkDescriptorType nv12Types[5] = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    if (!setLayout(scaleTypes, 2, d->scaleLayout) || !setLayout(nv12Types, 5, d->nv12Layout)) {
        error = "Vulkan: no descriptor set layout";
        return false;
    }
    auto pipelineLayout = [&](VkDescriptorSetLayout set, uint32_t pushBytes,
                              VkPipelineLayout& out) {
        VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo pli = {};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        return fn.vkCreatePipelineLayout(dev, &pli, nullptr, &out) == VK_SUCCESS;
    };
    if (!pipelineLayout(d->scaleLayout, sizeof(ScalePush), d->scalePipelineLayout) ||
        !pipelineLayout(d->nv12Layout, sizeof(Nv12Push), d->nv12PipelineLayout)) {
        error = "Vulkan: no pipeline layout";
        return false;
    }
    auto pipeline = [&](const uint32_t* code, size_t bytes, VkPipelineLayout layout,
                        VkPipeline& out) {
        VkShaderModule module = shaderModule(fn, dev, code, bytes);
        if (!module) return false;
        VkComputePipelineCreateInfo cpi = {};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = layout;
        const VkResult r = fn.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &out);
        fn.vkDestroyShaderModule(dev, module, nullptr);
        return r == VK_SUCCESS;
    };
    if (!pipeline(kVkNv12Spv, sizeof(kVkNv12Spv), d->nv12PipelineLayout, d->nv12) ||
        !pipeline(kVkScaleHSpv, sizeof(kVkScaleHSpv), d->scalePipelineLayout, d->scaleH) ||
        !pipeline(kVkScaleVSpv, sizeof(kVkScaleVSpv), d->scalePipelineLayout, d->scaleV)) {
        error = "Vulkan: a compute pipeline did not build";
        return false;
    }

    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dpi = {};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 3;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (fn.vkCreateDescriptorPool(dev, &dpi, nullptr, &d->descriptors) != VK_SUCCESS) {
        error = "Vulkan: no descriptor pool";
        return false;
    }
    const VkDescriptorSetLayout layouts[3] = {d->scaleLayout, d->scaleLayout, d->nv12Layout};
    VkDescriptorSet sets[3] = {};
    VkDescriptorSetAllocateInfo dsa = {};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = d->descriptors;
    dsa.descriptorSetCount = 3;
    dsa.pSetLayouts = layouts;
    if (fn.vkAllocateDescriptorSets(dev, &dsa, sets) != VK_SUCCESS) {
        error = "Vulkan: no descriptor sets";
        return false;
    }
    d->scaleHSet = sets[0];
    d->scaleVSet = sets[1];
    d->nv12Set = sets[2];

    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->device->family();
    VkCommandBufferAllocateInfo cai = {};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (fn.vkCreateCommandPool(dev, &pci, nullptr, &d->commands) != VK_SUCCESS ||
        (cai.commandPool = d->commands, fn.vkAllocateCommandBuffers(dev, &cai, &d->cmd)) !=
            VK_SUCCESS) {
        error = "Vulkan: no command buffer";
        return false;
    }
    if (d->device->importsSyncFile()) {
        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (fn.vkCreateSemaphore(dev, &semaphore, nullptr, &d->implicitFence) != VK_SUCCESS)
            d->implicitFence = VK_NULL_HANDLE;
    }

    // The pointer's two textures, one texel each until a shape arrives, so the
    // conversion's descriptors are whole from the first frame.
    VkResult r =
        createImage(*d->device, VK_FORMAT_B8G8R8A8_UNORM, 1, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d->cursorPixels);
    if (r == VK_SUCCESS)
        r = createImage(*d->device, VK_FORMAT_R8_UNORM, 1, 1,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        d->cursorInvert);
    if (r != VK_SUCCESS) {
        error = "Vulkan: the pointer's textures: " + resultText(r);
        return false;
    }
    VkDescriptorImageInfo infos[2] = {
        {d->linear, d->cursorPixels.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {d->nearest, d->cursorInvert.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = d->nv12Set;
        writes[i].dstBinding = 1 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    fn.vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    return true;
}

bool VulkanConvert::createScaler(std::string& error)
{
    VkResult r =
        createImage(*d->device, VK_FORMAT_R16G16B16A16_SFLOAT, m_PictureWidth, m_SourceHeight,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, d->mid);
    if (r == VK_SUCCESS)
        r = createImage(*d->device, VK_FORMAT_R8G8B8A8_UNORM, m_OutputWidth, m_OutputHeight,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, d->scaled);
    if (r != VK_SUCCESS) {
        error = "Vulkan: the resample pass's images: " + resultText(r);
        return false;
    }
    const VkDescriptorImageInfo midRead = {d->nearest, d->mid.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo midWrite = {VK_NULL_HANDLE, d->mid.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo scaledWrite = {VK_NULL_HANDLE, d->scaled.view,
                                               VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo scaledRead = {d->linear, d->scaled.view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[4] = {};
    auto write = [&](int i, VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                     const VkDescriptorImageInfo* info) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = type;
        writes[i].pImageInfo = info;
    };
    write(0, d->scaleHSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &midWrite);
    write(1, d->scaleVSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &midRead);
    write(2, d->scaleVSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &scaledWrite);
    // The conversion reads the scaled picture, linearly, as it would the
    // scanout: a 1:1 read lands on texel centres, and the chroma sample's read
    // at its own centre is a true 2x2 average.
    write(3, d->nv12Set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &scaledRead);
    d->fn->vkUpdateDescriptorSets(d->dev, 4, writes, 0, nullptr);

    // Without a GPU timer the cost cannot be known, and the pass stays: the
    // rule that drops it is about a measured cost, never a guessed one.
    if (d->device->timestamps()) {
        VkQueryPoolCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        if (d->fn->vkCreateQueryPool(d->dev, &qci, nullptr, &d->timestamps) != VK_SUCCESS)
            d->timestamps = VK_NULL_HANDLE;
    }
    if (!d->timestamps)
        log::info("[native] no GPU timer on this Vulkan queue — the resample pass is kept "
                  "without its cost measured");
    return true;
}

void VulkanConvert::releaseScaler()
{
    if (!d->device) return;
    destroyImage(*d->device, d->mid);
    destroyImage(*d->device, d->scaled);
    if (d->timestamps) d->fn->vkDestroyQueryPool(d->dev, d->timestamps, nullptr);
    d->timestamps = VK_NULL_HANDLE;
}

bool VulkanConvert::dropResample()
{
    if (m_Filter == ScaleFilter::Bilinear || m_Letterboxed) return false;
    // convert() branches on m_Filter alone, and binds the scene per frame: the
    // conversion samples the scanout again, as init() would have set it up.
    m_Filter = ScaleFilter::Bilinear;
    releaseScaler();
    return true;
}

bool VulkanConvert::takeResampleCost(int64_t& costUs)
{
    if (m_ResampleCostTaken || !m_ResampleCost.done()) return false;
    m_ResampleCostTaken = true;
    costUs = m_ResampleCost.medianUs();
    return true;
}

bool VulkanConvert::bindTarget(const Nv12Target& target, std::string& error)
{
    if (!d->device) {
        error = "Vulkan conversion is not initialized";
        return false;
    }
    if (target.width != m_OutputWidth || target.height != m_OutputHeight) {
        error = "the encoder's surface is not the size the converter produces";
        return false;
    }
    // One image per plane, as GlConvert imports one EGLImage per plane: R8 for
    // the luma, RG8 for the interleaved chroma at half size — written as
    // storage images, which the modifier must allow (VA-API's surface is
    // linear on AMD, where it does, bench §8o.4).
    VkFormatFeatureFlags2 lumaFeatures = 0, chromaFeatures = 0;
    uint32_t planes = 0;
    const bool lumaListed =
        d->device->modifierFeatures(VK_FORMAT_R8_UNORM, target.modifier, lumaFeatures, planes);
    const bool chromaListed =
        d->device->modifierFeatures(VK_FORMAT_R8G8_UNORM, target.modifier, chromaFeatures, planes);
    if (!lumaListed || !chromaListed || !(lumaFeatures & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) ||
        !(chromaFeatures & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)) {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx",
                      static_cast<unsigned long long>(target.modifier));
        error = std::string("Vulkan cannot write the encoder's surface (modifier ") + text +
                " is no storage image here)";
        return false;
    }
    const uint32_t offsetY = target.offsetY, pitchY = target.pitchY;
    const uint32_t offsetUV = target.offsetUV, pitchUV = target.pitchUV;
    VkResult r = importImage(*d->device, target.fdY, target.modifier, 1, &offsetY, &pitchY,
                             VK_FORMAT_R8_UNORM, target.width, target.height,
                             VK_IMAGE_USAGE_STORAGE_BIT, d->luma);
    if (r == VK_SUCCESS)
        r = importImage(*d->device, target.fdUV, target.modifier, 1, &offsetUV, &pitchUV,
                        VK_FORMAT_R8G8_UNORM, target.width / 2, target.height / 2,
                        VK_IMAGE_USAGE_STORAGE_BIT, d->chroma);
    if (r != VK_SUCCESS) {
        error = "Vulkan refused the encoder's NV12 planes: " + resultText(r);
        return false;
    }
    const VkDescriptorImageInfo infos[2] = {
        {VK_NULL_HANDLE, d->luma.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->chroma.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = d->nv12Set;
        writes[i].dstBinding = 3 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    d->fn->vkUpdateDescriptorSets(d->dev, 2, writes, 0, nullptr);
    d->targetBound = true;
    d->targetTouched = false;
    return true;
}

bool VulkanConvert::bindTarget(const encode::VulkanPicture& target, std::string& error)
{
    if (!d->device) {
        error = "Vulkan conversion is not initialized";
        return false;
    }
    if (target.width != m_OutputWidth || target.height != m_OutputHeight) {
        error = "the encoder's input is not the size the converter produces";
        return false;
    }
    if (!target.image || !target.luma || !target.chroma) {
        error = "the Vulkan encoder has no input to write";
        return false;
    }
    // Its own plane views, R8 and RG8 storage (VulkanHevcEncoder): the same
    // two bindings the imported planes take.
    const VkDescriptorImageInfo infos[2] = {
        {VK_NULL_HANDLE, target.luma, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, target.chroma, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = d->nv12Set;
        writes[i].dstBinding = 3 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    d->fn->vkUpdateDescriptorSets(d->dev, 2, writes, 0, nullptr);
    d->encoderInput = target.image;
    d->targetBound = true;
    d->targetTouched = false;
    return true;
}

bool VulkanConvert::updateCursor(const capture::CursorState& cursor, std::string& error)
{
    // New images when the size changed; the bytes land in the staging buffer,
    // and convert() records the copies ahead of the conversion.
    if (d->cursorPixels.width != cursor.width || d->cursorPixels.height != cursor.height) {
        destroyImage(*d->device, d->cursorPixels);
        destroyImage(*d->device, d->cursorInvert);
        VkResult r = createImage(*d->device, VK_FORMAT_B8G8R8A8_UNORM, cursor.width, cursor.height,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 d->cursorPixels);
        if (r == VK_SUCCESS)
            r = createImage(*d->device, VK_FORMAT_R8_UNORM, cursor.width, cursor.height,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            d->cursorInvert);
        if (r != VK_SUCCESS) {
            error = "Vulkan: the pointer's textures: " + resultText(r);
            return false;
        }
        const VkDescriptorImageInfo infos[2] = {
            {d->linear, d->cursorPixels.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {d->nearest, d->cursorInvert.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = d->nv12Set;
            writes[i].dstBinding = 1 + i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        d->fn->vkUpdateDescriptorSets(d->dev, 2, writes, 0, nullptr);
    }
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(cursor.width) * cursor.height;
    const VkDeviceSize bytes = pixels * 5; // BGRA, then the mask
    if (d->stagingSize < bytes) {
        const vulkan::DeviceFunctions& fn = *d->fn;
        if (d->staging) fn.vkDestroyBuffer(d->dev, d->staging, nullptr);
        if (d->stagingMemory) fn.vkFreeMemory(d->dev, d->stagingMemory, nullptr);
        d->staging = VK_NULL_HANDLE;
        d->stagingMemory = VK_NULL_HANDLE;
        d->stagingMapped = nullptr;
        d->stagingSize = 0;
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkMemoryRequirements req = {};
        VkResult r = fn.vkCreateBuffer(d->dev, &bci, nullptr, &d->staging);
        if (r == VK_SUCCESS) {
            fn.vkGetBufferMemoryRequirements(d->dev, d->staging, &req);
            VkMemoryAllocateInfo mai = {};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = req.size;
            mai.memoryTypeIndex =
                d->device->memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            r = mai.memoryTypeIndex == UINT32_MAX
                    ? VK_ERROR_OUT_OF_HOST_MEMORY
                    : fn.vkAllocateMemory(d->dev, &mai, nullptr, &d->stagingMemory);
        }
        if (r == VK_SUCCESS) r = fn.vkBindBufferMemory(d->dev, d->staging, d->stagingMemory, 0);
        if (r == VK_SUCCESS)
            r = fn.vkMapMemory(d->dev, d->stagingMemory, 0, VK_WHOLE_SIZE, 0, &d->stagingMapped);
        if (r != VK_SUCCESS) {
            error = "Vulkan: the pointer's staging buffer: " + resultText(r);
            return false;
        }
        d->stagingSize = bytes;
    }
    // BGRA as CursorState holds it: the texture is B8G8R8A8, so the shader
    // reads .rgb as RGB with nothing swapped here (GLES has no BGRA upload,
    // hence GlConvert's swap).
    auto* out = static_cast<uint8_t*>(d->stagingMapped);
    const size_t colour = std::min<size_t>(cursor.pixels.size(), pixels * 4);
    std::memcpy(out, cursor.pixels.data(), colour);
    if (colour < pixels * 4) std::memset(out + colour, 0, pixels * 4 - colour);
    const size_t mask = std::min<size_t>(cursor.invert.size(), pixels);
    std::memcpy(out + pixels * 4, cursor.invert.data(), mask);
    if (mask < pixels) std::memset(out + pixels * 4 + mask, 0, pixels - mask);
    return true;
}

bool VulkanConvert::convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                            const CursorDraw& draw, std::string& error)
{
    if (!d->device || !d->targetBound) {
        error = "Vulkan conversion has no target bound";
        return false;
    }
    if (d->failed || d->device->lost()) {
        error = "the Vulkan conversion was given up";
        return false;
    }
    auto fail = [&](std::string why) {
        d->failed = true;
        error = std::move(why);
        return false;
    };
    const vulkan::DeviceFunctions& fn = *d->fn;
    ++d->frames;
    if (d->failAt && d->frames == d->failAt)
        return fail("fault injected (MW_VK_CONVERT_FAIL_AT=" + std::to_string(d->failAt) + ")");

    // ── The scanout buffer, imported for this conversion only ──
    //
    // ⚠️ Never kept between frames. RADV from Mesa 26 sends every buffer the
    // device holds with every submission (its BO list is global, always), and
    // the kernel then syncs each submission — the Vulkan encoder's included —
    // with every scanout buffer still imported: the compositor writing the
    // game's next frame into the one after this. Kept imported (two or three
    // of them, the compositor's rotation), they cost the conversion 11 ms and
    // the encode 15 under a load that saturates the 780M, against 3.3 and 2.1
    // with RADV 25, whose list is per command buffer (bench §8o.6). An import
    // is 0.02 to 0.04 ms (§8o.2).
    // The portal's shared memory (C13.10): mapped pixels, no DMA-BUF.
    const bool fromHost = frame.mapped != nullptr;
    if (!fromHost && (frame.planeCount <= 0 || frame.fds[0] < 0))
        return fail("the frame has no DMA-BUF (shared memory is the CPU route's)");
    struct stat st = {};
    if (!fromHost && ::fstat(frame.fds[0], &st) != 0)
        return fail("the frame's DMA-BUF cannot be read");
    for (int i = 1; !fromHost && i < frame.planeCount; ++i) {
        struct stat other = {};
        if (frame.fds[i] < 0 || ::fstat(frame.fds[i], &other) != 0 || other.st_ino != st.st_ino ||
            other.st_dev != st.st_dev)
            return fail("the scanout's planes live in several objects, which this does not "
                        "import");
    }
    const VkFormat format = sourceFormat(frame.fourcc);
    if (fromHost && format == VK_FORMAT_UNDEFINED)
        return fail("the shared memory's format is not one this converts");
    SourceFormat& checked = d->sourceFormat;
    if (!fromHost && (!checked.usable || checked.fourcc != frame.fourcc ||
                      checked.modifier != frame.modifier || checked.planes != frame.planeCount)) {
        VkFormatFeatureFlags2 features = 0;
        uint32_t planes = 0;
        const bool listed = format != VK_FORMAT_UNDEFINED &&
                            d->device->modifierFeatures(format, frame.modifier, features, planes);
        // Filtered sampling whatever the path: a buffer kept for the resample
        // pass is sampled bilinear once dropResample() has given that pass up.
        const VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                                           VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (!listed || (features & need) != need ||
            planes != static_cast<uint32_t>(frame.planeCount)) {
            char text[48];
            std::snprintf(text, sizeof(text), "0x%llx",
                          static_cast<unsigned long long>(frame.modifier));
            return fail(std::string("Vulkan cannot sample the scanout buffer (modifier ") + text +
                        ", " + std::to_string(frame.planeCount) + " plane(s)" +
                        (listed ? ", driver wants " + std::to_string(planes) : ", not listed") +
                        ")");
        }
        checked = SourceFormat{frame.fourcc, frame.modifier, frame.planeCount, true};
    }
    Image sourceImage;
    // The portal's mapping, imported for this conversion (C13.10).
    VkBuffer hostBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hostMemory = VK_NULL_HANDLE;
    // Let go when this conversion returns, whatever it returns: past the CPU
    // wait below, the GPU is done with them.
    struct Release
    {
        vulkan::VulkanDevice& device;
        Image& image;
        VkBuffer& buffer;
        VkDeviceMemory& memory;
        ~Release()
        {
            destroyImage(device, image);
            if (buffer) device.fn().vkDestroyBuffer(device.device(), buffer, nullptr);
            if (memory) device.fn().vkFreeMemory(device.device(), memory, nullptr);
        }
    } release{*d->device, sourceImage, hostBuffer, hostMemory};
    // Where the shared memory's pixels are read from, and from which byte.
    VkBuffer hostSource = VK_NULL_HANDLE;
    VkDeviceSize hostOffset = 0;
    if (fromHost) {
        // ── The portal's shared memory: into a picture of ours first ──
        //
        // Read where it is mapped where the driver imports the mapping — the
        // GPU copies it, the CPU never touches a pixel. A driver that refuses
        // it (amdgpu takes anonymous memory only, and PipeWire's is a memfd)
        // gets the CPU's copy into a buffer of ours, still a fraction of what
        // the CPU pair costs: the conversion and the encode stay on the GPU.
        // As the compositor holds it: a picture turned against its panel stays
        // turned in our copy, and is stood upright where it is sampled.
        const int heldWidth = frame.importWidth();
        const int heldHeight = frame.importHeight();
        const size_t bytes = static_cast<size_t>(frame.pitches[0]) * heldHeight;
        if (frame.pitches[0] % 4 != 0 || frame.mappedSize < bytes)
            return fail("the shared memory's rows do not hold the picture");
        if (!d->hostPicture.image || d->hostPicture.width != heldWidth ||
            d->hostPicture.height != heldHeight || d->hostPictureFormat != format) {
            destroyImage(*d->device, d->hostPicture);
            const VkResult r = createImage(
                *d->device, format, heldWidth, heldHeight,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d->hostPicture);
            if (r != VK_SUCCESS) return fail("the shared memory's picture: " + resultText(r));
            d->hostPictureFormat = format;
        }
        std::string how;
        if (!d->hostImportRefused && d->device->importsHostMemory()) {
            const VkResult r = importHostMemory(*d->device, frame.mapped, bytes, hostBuffer,
                                                hostMemory, hostOffset);
            if (r == VK_SUCCESS) {
                hostSource = hostBuffer;
                how = "imported where it is mapped (VK_EXT_external_memory_host)";
            } else {
                d->hostImportRefused = true;
                log::info("[native] Vulkan conversion of the portal's shared memory: the driver "
                          "refused the mapping (" +
                          resultText(r) + ") — the CPU copies it from here");
                if (hostBuffer) d->fn->vkDestroyBuffer(d->dev, hostBuffer, nullptr);
                if (hostMemory) d->fn->vkFreeMemory(d->dev, hostMemory, nullptr);
                hostBuffer = VK_NULL_HANDLE;
                hostMemory = VK_NULL_HANDLE;
                hostOffset = 0;
            }
        } else if (!d->device->importsHostMemory()) {
            how = "copied by the CPU: no VK_EXT_external_memory_host here";
        }
        if (!hostSource) {
            if (d->hostCopySize < bytes) {
                if (d->hostCopy) fn.vkDestroyBuffer(d->dev, d->hostCopy, nullptr);
                if (d->hostCopyMemory) fn.vkFreeMemory(d->dev, d->hostCopyMemory, nullptr);
                d->hostCopy = VK_NULL_HANDLE;
                d->hostCopyMemory = VK_NULL_HANDLE;
                d->hostCopyMapped = nullptr;
                d->hostCopySize = 0;
                VkBufferCreateInfo bci = {};
                bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bci.size = bytes;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkResult r = fn.vkCreateBuffer(d->dev, &bci, nullptr, &d->hostCopy);
                VkMemoryRequirements req = {};
                if (r == VK_SUCCESS) fn.vkGetBufferMemoryRequirements(d->dev, d->hostCopy, &req);
                const uint32_t type = d->device->memoryType(
                    req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (r == VK_SUCCESS && type == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                if (r == VK_SUCCESS) {
                    VkMemoryAllocateInfo mai = {};
                    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    mai.allocationSize = req.size;
                    mai.memoryTypeIndex = type;
                    r = fn.vkAllocateMemory(d->dev, &mai, nullptr, &d->hostCopyMemory);
                }
                if (r == VK_SUCCESS)
                    r = fn.vkBindBufferMemory(d->dev, d->hostCopy, d->hostCopyMemory, 0);
                if (r == VK_SUCCESS)
                    r = fn.vkMapMemory(d->dev, d->hostCopyMemory, 0, VK_WHOLE_SIZE, 0,
                                       &d->hostCopyMapped);
                if (r != VK_SUCCESS)
                    return fail("the shared memory's copy buffer: " + resultText(r));
                d->hostCopySize = bytes;
            }
            std::memcpy(d->hostCopyMapped, frame.mapped, bytes);
            hostSource = d->hostCopy;
            hostOffset = 0;
        }
        if (!d->saidHostPath && !how.empty()) {
            d->saidHostPath = true;
            log::info("[native] Vulkan conversion of the portal's shared memory: " + how);
        }
    } else {
        // At the buffer's size: a screen of an X11 desktop over several reads
        // its window of it (sourceRect and the scale pass's offset, below).
        const VkResult imported =
            importImage(*d->device, frame.fds[0], frame.modifier, frame.planeCount, frame.offsets,
                        frame.pitches, format, frame.importWidth(), frame.importHeight(),
                        VK_IMAGE_USAGE_SAMPLED_BIT, sourceImage);
        if (imported != VK_SUCCESS)
            return fail("Vulkan refused the scanout buffer: " + resultText(imported));
    }
    const VkImageView sourceView = fromHost ? d->hostPicture.view : sourceImage.view;

    // ── The pointer: new bytes when its shape changed ──
    const bool upload = cursor.width > 0 && cursor.height > 0 &&
                        (!d->haveCursor || m_CursorShapeVersion != cursor.shapeVersion);
    if (upload && !updateCursor(cursor, error)) {
        d->failed = true;
        return false;
    }

    // ── The scene the conversion samples: the scanout, or the scaled picture ──
    const bool resampled = m_Filter != ScaleFilter::Bilinear;
    {
        VkDescriptorImageInfo sourceRead = {resampled ? d->nearest : d->linear, sourceView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = resampled ? d->scaleHSet : d->nv12Set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &sourceRead;
        fn.vkUpdateDescriptorSets(d->dev, 1, &w, 0, nullptr);
    }
    const bool timed = resampled && d->timestamps && m_ResampleCost.timeThisFrame(0);

    // ── The cursor rectangle, GlConvert's arithmetic ──
    // A shape uploaded by this very command buffer is drawable in it: the copy
    // is recorded ahead of the conversion, as GlConvert uploads before it draws.
    const bool drawCursor = cursor.visible && (d->haveCursor || upload) && cursor.width > 0 &&
                            cursor.height > 0 && m_SourceWidth > 0 && m_SourceHeight > 0;
    const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
    const float cw = static_cast<float>(cursor.width) * magnify;
    const float ch = static_cast<float>(cursor.height) * magnify;
    const float cx =
        static_cast<float>(cursor.x + draw.hotspotX) - static_cast<float>(draw.hotspotX) * magnify;
    const float cy =
        static_cast<float>(cursor.y + draw.hotspotY) - static_cast<float>(draw.hotspotY) * magnify;
    Nv12Push push = {};
    push.cursorRect[0] = cx / static_cast<float>(m_SourceWidth);
    push.cursorRect[1] = cy / static_cast<float>(m_SourceHeight);
    push.cursorRect[2] = cw / static_cast<float>(m_SourceWidth);
    push.cursorRect[3] = ch / static_cast<float>(m_SourceHeight);
    if (resampled) {
        const float sx = static_cast<float>(m_PictureWidth) / static_cast<float>(m_OutputWidth);
        const float sy = static_cast<float>(m_PictureHeight) / static_cast<float>(m_OutputHeight);
        push.cursorRect[0] = static_cast<float>(m_PictureX) / static_cast<float>(m_OutputWidth) +
                             push.cursorRect[0] * sx;
        push.cursorRect[1] = static_cast<float>(m_PictureY) / static_cast<float>(m_OutputHeight) +
                             push.cursorRect[1] * sy;
        push.cursorRect[2] *= sx;
        push.cursorRect[3] *= sy;
    }
    push.sourceRect[0] = push.sourceRect[1] = 0.0f;
    push.sourceRect[2] = push.sourceRect[3] = 1.0f;
    // The window is the picture as the input holds it, turned or not.
    if (!resampled && !fromHost &&
        (frame.sourceX || frame.sourceY || frame.importWidth() != frame.windowWidth() ||
         frame.importHeight() != frame.windowHeight())) {
        const float bw = static_cast<float>(frame.importWidth());
        const float bh = static_cast<float>(frame.importHeight());
        push.sourceRect[0] = static_cast<float>(frame.sourceX) / bw;
        push.sourceRect[1] = static_cast<float>(frame.sourceY) / bh;
        push.sourceRect[2] = static_cast<float>(frame.windowWidth()) / bw;
        push.sourceRect[3] = static_cast<float>(frame.windowHeight()) / bh;
    }
    push.size[0] = m_OutputWidth;
    push.size[1] = m_OutputHeight;
    push.cursorEnabled = drawCursor ? 1.0f : 0.0f;
    // A display turned against its panel (issue #54): stood upright as it is
    // read — by the horizontal pass when there is one, else here.
    const int32_t turns = frame.orientation.turns & 3;
    const int32_t mirrored = frame.orientation.mirrored ? 1 : 0;
    push.orient[0] = resampled ? 0 : turns;
    push.orient[1] = resampled ? 0 : mirrored;

    // ── Record ──
    VkCommandBuffer cmd = d->cmd;
    fn.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(cmd, &cbi);
    const uint32_t family = d->device->family();
    auto barriers = [&](const std::vector<VkImageMemoryBarrier2>& list) {
        if (list.empty()) return;
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(list.size());
        dep.pImageMemoryBarriers = list.data();
        fn.vkCmdPipelineBarrier2(cmd, &dep);
    };
    constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkAccessFlags2 kSampled = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    constexpr VkAccessFlags2 kWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    if (timed) fn.vkCmdResetQueryPool(cmd, d->timestamps, 0, 2);
    std::vector<VkImageMemoryBarrier2> before;
    if (fromHost) {
        // The shared memory's picture: written whole by the copy below.
        before.push_back(imageBarrier(
            d->hostPicture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT));
    } else {
        // The scanout, taken over from the compositor (the foreign queue
        // family): what it drew is visible once its implicit fence, waited on
        // below, has signalled.
        before.push_back(imageBarrier(sourceImage.image, VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kCompute,
                                      kSampled, VK_QUEUE_FAMILY_FOREIGN_EXT, family));
    }
    if (d->encoderInput) {
        // The Vulkan encoder's input, from where the encoder leaves it —
        // after the encode that read it, which the submission waits for at
        // this stage. Its contents are kept: the black past the picture.
        before.push_back(imageBarrier(d->encoderInput, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
                                      VK_IMAGE_LAYOUT_GENERAL, kCompute, VK_ACCESS_2_NONE, kCompute,
                                      kWrite));
    } else {
        // The encoder's planes: from VA-API after the first frame, which is
        // what read them last; overwritten whole, so the first time needs no
        // contents.
        for (Image* plane : {&d->luma, &d->chroma}) {
            before.push_back(
                d->targetTouched
                    ? imageBarrier(plane->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kCompute, kWrite,
                                   VK_QUEUE_FAMILY_FOREIGN_EXT, family)
                    : imageBarrier(plane->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kCompute, kWrite));
        }
    }
    if (upload) {
        for (Image* image : {&d->cursorPixels, &d->cursorInvert})
            before.push_back(
                imageBarrier(image->image, image->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
    } else if (d->cursorPixels.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        // The one-texel placeholders, never written: readable all the same.
        for (Image* image : {&d->cursorPixels, &d->cursorInvert}) {
            before.push_back(imageBarrier(
                image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kCompute, kSampled));
            image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    if (resampled)
        before.push_back(imageBarrier(d->mid.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE,
                                      VK_ACCESS_2_NONE, kCompute, kWrite));
    barriers(before);

    if (fromHost) {
        // The shared memory into the picture the conversion samples: the
        // submission makes the host's writes visible to the copy.
        VkBufferImageCopy copy = {};
        copy.bufferOffset = hostOffset;
        copy.bufferRowLength = frame.pitches[0] / 4;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<uint32_t>(frame.importWidth()),
                            static_cast<uint32_t>(frame.importHeight()), 1};
        fn.vkCmdCopyBufferToImage(cmd, hostSource, d->hostPicture.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barriers(
            {imageBarrier(d->hostPicture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kSampled)});
    }

    if (upload) {
        const uint32_t w = static_cast<uint32_t>(cursor.width);
        const uint32_t h = static_cast<uint32_t>(cursor.height);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        fn.vkCmdCopyBufferToImage(cmd, d->staging, d->cursorPixels.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = static_cast<VkDeviceSize>(w) * h * 4;
        fn.vkCmdCopyBufferToImage(cmd, d->staging, d->cursorInvert.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        std::vector<VkImageMemoryBarrier2> uploaded;
        for (Image* image : {&d->cursorPixels, &d->cursorInvert}) {
            uploaded.push_back(imageBarrier(image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_PIPELINE_STAGE_2_COPY_BIT,
                                            VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kSampled));
            image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        barriers(uploaded);
    }

    // ── The resample pass: horizontal, then vertical ──
    if (resampled) {
        if (timed)
            fn.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, d->timestamps, 0);
        const double dilateH = std::max(1.0, static_cast<double>(m_SourceWidth) / m_PictureWidth);
        const double dilateV = std::max(1.0, static_cast<double>(m_SourceHeight) / m_PictureHeight);
        ScalePush h = {};
        h.origin[0] = h.origin[1] = 0;
        h.size[0] = h.extent[0] = m_PictureWidth;
        h.size[1] = h.extent[1] = m_SourceHeight;
        if (!fromHost) {
            h.offset[0] = frame.sourceX;
            h.offset[1] = frame.sourceY;
        }
        h.len = static_cast<float>(m_SourceWidth);
        h.fixedLen = static_cast<float>(m_SourceHeight);
        // GlConvert writes the dilation as the ratio "(len.0 / out.0)" or
        // "1.0": the float nearest the ratio, which this cast is too.
        h.dilate = static_cast<float>(dilateH);
        h.taps = static_cast<int32_t>(std::ceil(4.0 * dilateH)) + 1;
        h.orient[0] = turns;
        h.orient[1] = mirrored;
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->scaleH);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->scalePipelineLayout, 0,
                                   1, &d->scaleHSet, 0, nullptr);
        fn.vkCmdPushConstants(cmd, d->scalePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(h), &h);
        fn.vkCmdDispatch(cmd, (static_cast<uint32_t>(m_PictureWidth) + 15) / 16,
                         (static_cast<uint32_t>(m_SourceHeight) + 15) / 16, 1);
        barriers({imageBarrier(d->mid.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute, kWrite, kCompute,
                               kSampled),
                  imageBarrier(d->scaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, kCompute, kWrite)});
        ScalePush v = {};
        v.origin[0] = m_PictureX;
        v.origin[1] = m_PictureY;
        v.size[0] = m_PictureWidth;
        v.size[1] = m_PictureHeight;
        v.extent[0] = m_OutputWidth;
        v.extent[1] = m_OutputHeight;
        v.len = static_cast<float>(m_SourceHeight);
        v.fixedLen = static_cast<float>(m_PictureWidth);
        v.dilate = static_cast<float>(dilateV);
        v.taps = static_cast<int32_t>(std::ceil(4.0 * dilateV)) + 1;
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->scaleV);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->scalePipelineLayout, 0,
                                   1, &d->scaleVSet, 0, nullptr);
        fn.vkCmdPushConstants(cmd, d->scalePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(v), &v);
        fn.vkCmdDispatch(cmd, (static_cast<uint32_t>(m_OutputWidth) + 15) / 16,
                         (static_cast<uint32_t>(m_OutputHeight) + 15) / 16, 1);
        barriers({imageBarrier(d->scaled.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute, kWrite, kCompute,
                               kSampled)});
        if (timed)
            fn.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, d->timestamps, 1);
    }

    // ── The conversion into the encoder's planes ──
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->nv12);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->nv12PipelineLayout, 0, 1,
                               &d->nv12Set, 0, nullptr);
    fn.vkCmdPushConstants(cmd, d->nv12PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                          &push);
    fn.vkCmdDispatch(cmd, (static_cast<uint32_t>(m_OutputWidth) + 15) / 16,
                     (static_cast<uint32_t>(m_OutputHeight) + 15) / 16, 1);

    // ── Handed back: the planes to VA-API (or the input to the Vulkan
    // encoder, whose submission waits on this one's), the scanout to the
    // compositor ──
    std::vector<VkImageMemoryBarrier2> after;
    if (d->encoderInput) {
        after.push_back(imageBarrier(d->encoderInput, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, kCompute, kWrite,
                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE));
    } else {
        for (Image* plane : {&d->luma, &d->chroma})
            after.push_back(imageBarrier(
                plane->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kCompute, kWrite,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, family, VK_QUEUE_FAMILY_FOREIGN_EXT));
    }
    if (!fromHost)
        after.push_back(imageBarrier(sourceImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_IMAGE_LAYOUT_GENERAL, kCompute, kSampled,
                                     VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, family,
                                     VK_QUEUE_FAMILY_FOREIGN_EXT));
    barriers(after);
    fn.vkEndCommandBuffer(cmd);

    // ── The scanout's implicit fence, as a sync_file the queue waits on — and,
    // writing the Vulkan encoder's input, the encode that read it last ──
    VkSemaphoreSubmitInfo waitList[2] = {};
    uint32_t listed = 0;
    if (d->encoderInput) {
        const VkSemaphoreSubmitInfo previous = d->device->afterLast(kCompute);
        if (previous.semaphore) waitList[listed++] = previous;
    }
    VkSemaphoreSubmitInfo wait = {};
    uint32_t waits = 0;
    // Shared memory has no fence: the compositor finished writing it before
    // it handed the buffer over.
    if (d->implicitFence && !fromHost) {
        struct dma_buf_export_sync_file exported = {};
        exported.flags = DMA_BUF_SYNC_READ;
        exported.fd = -1;
        if (::ioctl(frame.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 &&
            exported.fd >= 0) {
            VkImportSemaphoreFdInfoKHR isi = {};
            isi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            isi.semaphore = d->implicitFence;
            isi.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            isi.fd = exported.fd;
            if (fn.vkImportSemaphoreFdKHR(d->dev, &isi) == VK_SUCCESS) {
                wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                wait.semaphore = d->implicitFence;
                wait.stageMask = kCompute;
                waits = 1;
            } else {
                ::close(exported.fd);
            }
        }
    }
    if (!waits && !fromHost && !d->saidNoFence) {
        d->saidNoFence = true;
        log::info("[native] the scanout's implicit fence is not waited on (no sync_file from "
                  "the kernel or the driver): the buffer is read as it stands");
    }
    if (waits) waitList[listed++] = wait;
    std::string why;
    if (!d->device->run(cmd, listed ? waitList : nullptr, listed, why))
        return fail("the Vulkan conversion failed: " + why);
    d->targetTouched = true;
    if (upload) {
        d->haveCursor = true;
        m_CursorShapeVersion = cursor.shapeVersion;
    }

    if (timed) {
        uint64_t stamps[2] = {};
        if (fn.vkGetQueryPoolResults(d->dev, d->timestamps, 0, 2, sizeof(stamps), stamps,
                                     sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] >= stamps[0]) {
            const double ns =
                static_cast<double>(stamps[1] - stamps[0]) * d->device->timestampPeriod();
            m_ResampleCost.add(static_cast<int64_t>(ns / 1000.0));
        }
    }
    return true;
}

bool VulkanConvert::highPriority() const
{
    return d && d->device && d->device->highPriority();
}

bool VulkanConvert::lost() const
{
    return d && (d->failed || (d->device && d->device->lost()));
}

void VulkanConvert::stop()
{
    if (!d) return;
    if (d->device) {
        vulkan::VulkanDevice& device = *d->device;
        const vulkan::DeviceFunctions& fn = device.fn();
        VkDevice dev = device.device();
        if (fn.vkDeviceWaitIdle) fn.vkDeviceWaitIdle(dev);
        releaseScaler();
        destroyImage(device, d->luma);
        destroyImage(device, d->chroma);
        destroyImage(device, d->cursorPixels);
        destroyImage(device, d->cursorInvert);
        destroyImage(device, d->hostPicture);
        if (d->staging) fn.vkDestroyBuffer(dev, d->staging, nullptr);
        if (d->stagingMemory) fn.vkFreeMemory(dev, d->stagingMemory, nullptr);
        if (d->hostCopy) fn.vkDestroyBuffer(dev, d->hostCopy, nullptr);
        if (d->hostCopyMemory) fn.vkFreeMemory(dev, d->hostCopyMemory, nullptr);
        if (d->implicitFence) fn.vkDestroySemaphore(dev, d->implicitFence, nullptr);
        if (d->commands) fn.vkDestroyCommandPool(dev, d->commands, nullptr);
        if (d->descriptors) fn.vkDestroyDescriptorPool(dev, d->descriptors, nullptr);
        for (VkPipeline p : {d->scaleH, d->scaleV, d->nv12})
            if (p) fn.vkDestroyPipeline(dev, p, nullptr);
        for (VkPipelineLayout l : {d->scalePipelineLayout, d->nv12PipelineLayout})
            if (l) fn.vkDestroyPipelineLayout(dev, l, nullptr);
        for (VkDescriptorSetLayout l : {d->scaleLayout, d->nv12Layout})
            if (l) fn.vkDestroyDescriptorSetLayout(dev, l, nullptr);
        for (VkSampler s : {d->linear, d->nearest})
            if (s) fn.vkDestroySampler(dev, s, nullptr);
    }
    d = std::make_unique<Impl>();
    m_Priority.clear();
}

} // namespace mw::native::convert
