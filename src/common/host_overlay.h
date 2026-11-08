// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Common {

/// An image the emulator itself draws over the game's picture (game mods such as the Pokopia
/// minimap). Premultiplied RGBA8, Width x Height, stretched over the game's image when presented.
class HostOverlay {
public:
    static constexpr u32 Width = 960;
    static constexpr u32 Height = 540;
    static constexpr std::size_t Size = std::size_t{Width} * Height * 4;

    /// Presented only while enabled; a mod enables it when it starts drawing.
    void SetEnabled(bool on) {
        enabled.store(on, std::memory_order_release);
    }
    bool Enabled() const {
        return enabled.load(std::memory_order_acquire);
    }

    /// Replaces the image (Size bytes).
    void Publish(std::span<const u8> rgba) {
        std::scoped_lock lock{mutex};
        pixels.assign(rgba.begin(), rgba.end());
        pixels.resize(Size);
    }

    /// Copies the image into `out` (Size bytes); transparent before the first Publish.
    void CopyTo(std::span<u8> out) {
        std::scoped_lock lock{mutex};
        if (pixels.size() == Size && out.size() >= Size) {
            std::memcpy(out.data(), pixels.data(), Size);
        } else {
            std::memset(out.data(), 0, std::min(out.size(), Size));
        }
    }

private:
    std::atomic<bool> enabled{false};
    std::mutex mutex;
    std::vector<u8> pixels;
};

inline HostOverlay& GetHostOverlay() {
    static HostOverlay overlay;
    return overlay;
}

} // namespace Common
