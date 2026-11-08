// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/math_util.h"
#include "core/hle/service/nvnflinger/buffer_transform_flags.h"
#include "core/hle/service/nvnflinger/hwc_layer.h"
#include "core/hle/service/nvnflinger/pixel_format.h"
#include "core/hle/service/nvnflinger/ui/fence.h"

namespace Tegra {

enum class BlendMode {
    Opaque,
    Premultiplied,
    Coverage,
};

/**
 * Struct describing framebuffer configuration
 */
struct FramebufferConfig {
    DAddr address{};
    u32 offset{};
    u32 width{};
    u32 height{};
    u32 stride{};
    u64 allocation_size{};
    std::optional<u32> block_height_log2{};
    Service::android::PixelFormat pixel_format{};
    Service::android::BufferTransformFlags transform_flags{};
    Common::Rectangle<int> crop_rect{};
    BlendMode blending{};
    bool native_tiling{};
    u32 layer_stack_mask{Service::Nvnflinger::DefaultLayerStackMask};
    /// Not guest memory: the emulator's own overlay (common/host_overlay.h), linear RGBA8.
    bool host_overlay{};
    /// Shown only in this part of the screen (0..1: left, top, right, bottom), without filters,
    /// and only if the texture cache holds the image (no guest-memory fallback).
    std::optional<std::array<float, 4>> dest_rect{};
};

Common::Rectangle<f32> NormalizeCrop(const FramebufferConfig& framebuffer, u32 texture_width,
                                     u32 texture_height);

/**
 * Returns the subset of layers belonging to a stack.
 */
std::span<const FramebufferConfig> FilterLayerStack(std::span<const FramebufferConfig> layers,
                                                    Service::Nvnflinger::LayerStackId stack,
                                                    std::vector<FramebufferConfig>& scratch);

} // namespace Tegra
