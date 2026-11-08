// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <variant>
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/anti_alias_pass.h"
/* X11 defines */
#undef Success
#undef BadValue
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#include "common/host_overlay.h"
#include "common/settings.h"
#include "video_core/framebuffer_config.h"
#include "video_core/renderer_vulkan/present/fsr.h"
#include "video_core/renderer_vulkan/present/fsr3.h"
#include "video_core/renderer_vulkan/present/sgsr.h"
#include "video_core/renderer_vulkan/present/fxaa.h"
#include "video_core/renderer_vulkan/present/layer.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#include "video_core/renderer_vulkan/present/post_process.h"
#endif
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/smaa.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/textures/decoders.h"

namespace Vulkan {

namespace {

u32 GetBytesPerPixel(const Tegra::FramebufferConfig& framebuffer) {
    using namespace VideoCore::Surface;
    return BytesPerBlock(PixelFormatFromGPUPixelFormat(framebuffer.pixel_format));
}

std::size_t GetSizeInBytes(const Tegra::FramebufferConfig& framebuffer) {
    return static_cast<std::size_t>(framebuffer.stride) *
           static_cast<std::size_t>(framebuffer.height) * GetBytesPerPixel(framebuffer);
}

VkFormat GetFormat(const Tegra::FramebufferConfig& framebuffer) {
    switch (framebuffer.pixel_format) {
    case Service::android::PixelFormat::Rgba8888:
    case Service::android::PixelFormat::Rgbx8888:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case Service::android::PixelFormat::Rgb565:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Service::android::PixelFormat::Bgra8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case Service::android::PixelFormat::Rgba1010102:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default:
        UNIMPLEMENTED_MSG("Unknown framebuffer pixel format: {}",
                          static_cast<u32>(framebuffer.pixel_format));
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    }
}

} // Anonymous namespace

Layer::Layer(const Device& device, MemoryAllocator& memory_allocator_, Scheduler& scheduler_, Tegra::MaxwellDeviceMemoryManager& device_memory_, size_t image_count_, VkExtent2D output_size, VkDescriptorSetLayout layout, const PresentFilters& filters_)
    : memory_allocator(memory_allocator_)
    , scheduler(scheduler_)
    , device_memory(device_memory_)
    , filters(filters_)
    , image_count(image_count_)
{
    CreateDescriptorPool(device);
    CreateDescriptorSets(device, layout);
    if (filters.get_scaling_filter() == Settings::ScalingFilter::Fsr) {
        sr_filter.emplace<FSR>(device, memory_allocator, image_count, output_size);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::Sgsr) {
        sr_filter.emplace<SGSR>(device, memory_allocator, image_count, output_size, false);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::SgsrEdge) {
        sr_filter.emplace<SGSR>(device, memory_allocator, image_count, output_size, true);
    } else if (filters.get_scaling_filter() == Settings::ScalingFilter::Fsr3) {
        sr_filter.emplace<FSR3>(device, memory_allocator, image_count, output_size);
    }
}

Layer::~Layer() {
    ReleaseRawImages();
}

void Layer::ConfigureDraw(const Device& device, PresentPushConstants* out_push_constants,
                          VkDescriptorSet* out_descriptor_set, RasterizerVulkan& rasterizer,
                          VkSampler sampler, size_t image_index,
                          const Tegra::FramebufferConfig& framebuffer,
                          const Layout::FramebufferLayout& layout) {
    const bool host_image = framebuffer.host_overlay || framebuffer.host_fill.has_value();
    const bool modded = host_image || framebuffer.dest_rect.has_value();
    const auto texture_info =
        host_image
            ? std::nullopt
            : rasterizer.AccelerateDisplay(framebuffer, framebuffer.address + framebuffer.offset,
                                           framebuffer.stride);
    // A find_by_size layer draws its own copy (raw images), of the framebuffer's size.
    const bool own_copy = framebuffer.own_copy;
    const bool from_cache = texture_info.has_value() && !own_copy;
    const u32 texture_width = from_cache ? texture_info->width : framebuffer.width;
    const u32 texture_height = from_cache ? texture_info->height : framebuffer.height;
    const u32 scaled_width = from_cache ? texture_info->scaled_width : texture_width;
    const u32 scaled_height = from_cache ? texture_info->scaled_height : texture_height;
    bool use_accelerated = texture_info.has_value();
    const bool is_applet =
        (framebuffer.layer_stack_mask & Service::Nvnflinger::LayerStackBit(
                                            Service::Nvnflinger::LayerStackId::Recording)) == 0;

    RefreshResources(device, framebuffer);
    if (!modded) {
        SetAntiAliasPass(device);
#ifdef HAS_RESHADE
        SetPostProcessPass(device, is_applet);
#endif
    }

    // Finish any pending renderpass
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Wait(resource_ticks[image_index]);
    SCOPE_EXIT {
        resource_ticks[image_index] = scheduler.CurrentTick();
    };

    if (!use_accelerated && (!framebuffer.dest_rect || framebuffer.host_fill)) {
        UpdateRawImage(framebuffer, image_index);
    }
    if (own_copy) {
        if (copy_generation.size() != image_count) {
            copy_generation.assign(image_count, 0);
            latest_copy_generation = 0;
        }
        if (texture_info) {
            CopyIntoRawImage(texture_info->image, texture_info->scaled_width,
                             texture_info->scaled_height, image_index);
            copy_generation[image_index] = ++latest_copy_generation;
            latest_copy_index = image_index;
        } else if (latest_copy_generation != 0 &&
                   copy_generation[image_index] < latest_copy_generation &&
                   latest_copy_index != image_index) {
            CopyIntoRawImage(*raw_images[latest_copy_index], raw_width, raw_height, image_index);
            copy_generation[image_index] = latest_copy_generation;
        }
        use_accelerated = copy_generation[image_index] != 0;
    }

    VkImage source_image = from_cache ? texture_info->image : *raw_images[image_index];
    VkImageView source_image_view =
        from_cache ? texture_info->image_view : *raw_image_views[image_index];

    if (auto* fxaa = std::get_if<FXAA>(&anti_alias)) {
        fxaa->Draw(device, scheduler, image_index, &source_image, &source_image_view);
    } else if (auto* smaa = std::get_if<SMAA>(&anti_alias)) {
        smaa->Draw(device, scheduler, image_index, &source_image, &source_image_view);
    }

#ifdef HAS_RESHADE
    if (post_process.has_value()) {
        post_process->Draw(device, scheduler, image_index, &source_image, &source_image_view);
    }
#endif

    auto crop_rect = Tegra::NormalizeCrop(framebuffer, texture_width, texture_height);
    const VkExtent2D render_extent{
        .width = scaled_width,
        .height = scaled_height,
    };

    if (modded) {
        // drawn as is: no upscaler
    } else if (auto* fsr = std::get_if<FSR>(&sr_filter)) {
        source_image_view = fsr->Draw(device, scheduler, image_index, source_image, source_image_view, render_extent, crop_rect);
        crop_rect = {0, 0, 1, 1};
    } else if (auto* sgsr = std::get_if<SGSR>(&sr_filter)) {
        if (!is_applet) {
            source_image_view = sgsr->Draw(device, scheduler, image_index, source_image,
                                           source_image_view, render_extent, crop_rect);
            crop_rect = {0, 0, 1, 1};
        }
    } else if (auto* fsr3 = std::get_if<FSR3>(&sr_filter)) {
        source_image_view = fsr3->Draw(device, scheduler, image_index, source_image,
                                       source_image_view, render_extent, crop_rect);
        crop_rect = {0, 0, 1, 1};
    }

    SetMatrixData(device, *out_push_constants, layout);
    SetVertexData(device, *out_push_constants, layout, crop_rect);
    if (framebuffer.dest_rect) {
        // A part of the screen; nothing at all when the texture cache does not hold the image.
        const auto& screen = layout.screen;
        const auto& d = *framebuffer.dest_rect;
        const bool drawable = use_accelerated || framebuffer.host_fill.has_value();
        const f32 w = drawable ? f32(screen.GetWidth()) : 0.f;
        const f32 h = drawable ? f32(screen.GetHeight()) : 0.f;
        const f32 x0 = f32(screen.left) + d[0] * w, x1 = f32(screen.left) + d[2] * w;
        const f32 y0 = f32(screen.top) + d[1] * h, y1 = f32(screen.top) + d[3] * h;
        auto& v = out_push_constants->vertices;
        v[0] = ScreenRectVertex(x0, y0, crop_rect.left, crop_rect.top);
        v[1] = ScreenRectVertex(x1, y0, crop_rect.right, crop_rect.top);
        v[2] = ScreenRectVertex(x0, y1, crop_rect.left, crop_rect.bottom);
        v[3] = ScreenRectVertex(x1, y1, crop_rect.right, crop_rect.bottom);
    }

    UpdateDescriptorSet(device, source_image_view, sampler, image_index);
    *out_descriptor_set = descriptor_sets[image_index];
}

void Layer::CreateDescriptorPool(const Device& device) {
    descriptor_pool = CreateWrappedDescriptorPool(device, image_count, image_count);
}

void Layer::CreateDescriptorSets(const Device& device, VkDescriptorSetLayout layout) {
    const std::vector layouts(image_count, layout);
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);
}

void Layer::CreateStagingBuffer(const Device& device, const Tegra::FramebufferConfig& framebuffer) {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = CalculateBufferSize(framebuffer),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };

    buffer = memory_allocator.CreateBuffer(ci, MemoryUsage::Upload);
}

void Layer::CreateRawImages(const Device& device, const Tegra::FramebufferConfig& framebuffer) {
    const auto format = GetFormat(framebuffer);
    resource_ticks.resize(image_count);
    raw_images.resize(image_count);
    raw_image_views.resize(image_count);

    for (size_t i = 0; i < image_count; ++i) {
        raw_images[i] =
            CreateWrappedImage(memory_allocator, {framebuffer.width, framebuffer.height}, format);
        raw_image_views[i] = CreateWrappedImageView(device, raw_images[i], format);
    }
}

void Layer::RefreshResources(const Device& device, const Tegra::FramebufferConfig& framebuffer) {
    if (framebuffer.width == raw_width && framebuffer.height == raw_height &&
        framebuffer.pixel_format == pixel_format && !raw_images.empty()) {
        return;
    }

    raw_width = framebuffer.width;
    raw_height = framebuffer.height;
    pixel_format = framebuffer.pixel_format;
    anti_alias.emplace<std::monostate>();

    ReleaseRawImages();
    copy_generation.clear();
    latest_copy_generation = 0;
    CreateStagingBuffer(device, framebuffer);
    CreateRawImages(device, framebuffer);
}

void Layer::SetAntiAliasPass(const Device& device) {
    if (!std::holds_alternative<std::monostate>(anti_alias) && anti_alias_setting == filters.get_anti_aliasing())
        return;

    anti_alias_setting = filters.get_anti_aliasing();

    const VkExtent2D render_area{
        .width = Settings::values.resolution_info.ScaleUp(raw_width),
        .height = Settings::values.resolution_info.ScaleUp(raw_height),
    };

    switch (anti_alias_setting) {
    case Settings::AntiAliasing::Fxaa:
        anti_alias.emplace<FXAA>(device, memory_allocator, image_count, render_area);
        break;
    case Settings::AntiAliasing::Smaa:
        anti_alias.emplace<SMAA>(device, memory_allocator, image_count, render_area);
        break;
    default:
        anti_alias.emplace<std::monostate>();
        break;
    }
}

#ifdef HAS_RESHADE
void Layer::SetPostProcessPass(const Device& device, bool is_applet) {
    if (is_applet) {
        post_process.reset();
        return;
    }
    const VkExtent2D render_area{
        .width = Settings::values.resolution_info.ScaleUp(raw_width),
        .height = Settings::values.resolution_info.ScaleUp(raw_height),
    };

    const u64 generation = VideoCore::FxChain::Instance().Snapshot().generation;
    const bool enabled = Settings::values.post_shader_enabled.GetValue();

    if (post_process_generation == generation && post_process_enabled == enabled &&
        post_process_extent.width == render_area.width &&
        post_process_extent.height == render_area.height) {
        return;
    }

    for (const u64 tick : resource_ticks) {
        scheduler.Wait(tick);
    }

    post_process_generation = generation;
    post_process_enabled = enabled;
    post_process_extent = render_area;
    post_process.reset();

    if (!enabled || VideoCore::FxChain::Instance().Size() == 0) {
        return;
    }

    post_process.emplace(device, memory_allocator, scheduler, image_count, render_area);

    if (post_process->Empty()) {
        post_process.reset();
    }
}
#endif

void Layer::ReleaseRawImages() {
    for (const u64 tick : resource_ticks) {
        scheduler.Wait(tick);
    }
    raw_images.clear();
    buffer.reset();
}

u64 Layer::CalculateBufferSize(const Tegra::FramebufferConfig& framebuffer) const {
    return GetSizeInBytes(framebuffer) * image_count;
}

u64 Layer::GetRawImageOffset(const Tegra::FramebufferConfig& framebuffer,
                             size_t image_index) const {
    return GetSizeInBytes(framebuffer) * image_index;
}

void Layer::SetMatrixData(const Device& device, PresentPushConstants& data, const Layout::FramebufferLayout& layout) const {
    data.modelview_matrix = MakeOrthographicMatrix(f32(layout.width), static_cast<f32>(layout.height));
}

void Layer::SetVertexData(const Device& device, PresentPushConstants& data, const Layout::FramebufferLayout& layout, const Common::Rectangle<f32>& crop) const {
    // Map the coordinates to the screen.
    const auto& screen = layout.screen;
    const auto x = f32(screen.left);
    const auto y = f32(screen.top);
    const auto w = f32(screen.GetWidth());
    const auto h = f32(screen.GetHeight());

    data.vertices[0] = ScreenRectVertex(x, y, crop.left, crop.top);
    data.vertices[1] = ScreenRectVertex(x + w, y, crop.right, crop.top);
    data.vertices[2] = ScreenRectVertex(x, y + h, crop.left, crop.bottom);
    data.vertices[3] = ScreenRectVertex(x + w, y + h, crop.right, crop.bottom);
}

void Layer::UpdateDescriptorSet(const Device& device, VkImageView image_view, VkSampler sampler, size_t image_index) {
    const VkDescriptorImageInfo image_info{
        .sampler = sampler,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };

    const VkWriteDescriptorSet sampler_write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_sets[image_index],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };

    device.GetLogical().UpdateDescriptorSets(std::array{sampler_write}, {});
}

void Layer::CopyIntoRawImage(VkImage src, u32 src_width, u32 src_height, size_t image_index) {
    const bool first = copy_generation[image_index] == 0;
    const VkImageBlit region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {static_cast<s32>(src_width), static_cast<s32>(src_height), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {static_cast<s32>(raw_width), static_cast<s32>(raw_height), 1}},
    };
    scheduler.Record([this, src, region, first, index = image_index](vk::CommandBuffer cmdbuf) {
        const VkImage dst = *raw_images[index];
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        const VkImageMemoryBarrier dst_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before, {}, dst_barrier);
        cmdbuf.BlitImage(src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, region,
                         VK_FILTER_LINEAR);
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, after);
    });
}

void Layer::UpdateRawImage(const Tegra::FramebufferConfig& framebuffer, size_t image_index) {
    const std::span<u8> mapped_span = buffer.Mapped();

    const u64 image_offset = GetRawImageOffset(framebuffer, image_index);

    const DAddr framebuffer_addr = framebuffer.address + framebuffer.offset;
    const u8* const host_ptr =
        (framebuffer.host_overlay || framebuffer.host_fill) ? nullptr : device_memory.GetPointer<u8>(framebuffer_addr);
    if (framebuffer.host_fill) {
        const u32 colour = *framebuffer.host_fill;
        std::memcpy(mapped_span.data() + image_offset, &colour, sizeof(colour));
        buffer.Flush();
    }
    if (framebuffer.host_overlay) {
        Common::GetHostOverlay().CopyTo(mapped_span.subspan(image_offset, GetSizeInBytes(framebuffer)));
        buffer.Flush();
    }

    // TODO(Rodrigo): Read this from HLE
    constexpr u32 block_height_log2 = 4;
    const u32 bytes_per_pixel = GetBytesPerPixel(framebuffer);
    const u64 linear_size{GetSizeInBytes(framebuffer)};
    const u64 tiled_size{Tegra::Texture::CalculateSize(
        true, bytes_per_pixel, framebuffer.stride, framebuffer.height, 1, block_height_log2, 0)};
    if (host_ptr) {
        Tegra::Texture::UnswizzleTexture(
            mapped_span.subspan(image_offset, linear_size), std::span(host_ptr, tiled_size),
            bytes_per_pixel, framebuffer.width, framebuffer.height, 1, block_height_log2, 0, 1, framebuffer.native_tiling);
        buffer.Flush();  // Ensure host writes are visible before the GPU copy.
    }

    const VkBufferImageCopy copy{
        .bufferOffset = image_offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent =
            {
                .width = framebuffer.width,
                .height = framebuffer.height,
                .depth = 1,
            },
    };
    scheduler.Record([this, copy, index = image_index](vk::CommandBuffer cmdbuf) {
        const VkImage image = *raw_images[index];
        const VkImageMemoryBarrier base_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkImageMemoryBarrier read_barrier = base_barrier;
        read_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        read_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        read_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkImageMemoryBarrier write_barrier = base_barrier;
        write_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        write_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        write_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               read_barrier);
        cmdbuf.CopyBufferToImage(*buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, write_barrier);
    });
}

} // namespace Vulkan
