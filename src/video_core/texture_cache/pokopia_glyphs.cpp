// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/texture_cache/pokopia_glyphs.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <bc7enc.h>
#include <bc_decoder.h>

#include "common/logging.h"

namespace VideoCommon::PokopiaGlyphs {

namespace {

constexpr int BlockBytes = 16;

struct Plan {
    std::mutex mutex;
    std::vector<Tile> atlas;
    std::vector<Tile> title;
};

Plan& P() {
    static Plan plan;
    return plan;
}

/// The texture's pixels in [x0, x1) x [y0, y1), widened to whole blocks.
struct Patch {
    int bx0, by0, bx1, by1; // blocks
    std::vector<u8> rgba;
    int Width() const {
        return (bx1 - bx0) * 4;
    }
    u8* At(int x, int y) {
        return rgba.data() + (static_cast<std::size_t>(y - by0 * 4) * Width() + (x - bx0 * 4)) * 4;
    }
};

Patch Decode(std::span<const u8> host, std::size_t row_bytes, int x0, int y0, int x1, int y1,
             int size) {
    Patch p;
    p.bx0 = std::max(0, x0 / 4);
    p.by0 = std::max(0, y0 / 4);
    p.bx1 = std::min(size / 4, (x1 + 3) / 4);
    p.by1 = std::min(size / 4, (y1 + 3) / 4);
    p.rgba.assign(static_cast<std::size_t>(p.Width()) * (p.by1 - p.by0) * 4 * 4, 0);
    u8 block[4 * 4 * 4];
    for (int by = p.by0; by < p.by1; ++by) {
        for (int bx = p.bx0; bx < p.bx1; ++bx) {
            const u8* src = host.data() + by * row_bytes + static_cast<std::size_t>(bx) * BlockBytes;
            bcn::DecodeBc7(src, block, 0, 0, 4, 4);
            for (int row = 0; row < 4; ++row) {
                std::copy_n(block + row * 16, 16, p.At(bx * 4, by * 4 + row));
            }
        }
    }
    return p;
}

void Encode(Patch& p, std::span<u8> host, std::size_t row_bytes) {
    static const bool ready = [] {
        bc7enc_compress_block_init();
        return true;
    }();
    (void)ready;
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_uber_level = 2;
    bc7enc_compress_block_params_init_linear_weights(&params);
    u8 block[4 * 4 * 4];
    for (int by = p.by0; by < p.by1; ++by) {
        for (int bx = p.bx0; bx < p.bx1; ++bx) {
            for (int row = 0; row < 4; ++row) {
                std::copy_n(p.At(bx * 4, by * 4 + row), 16, block + row * 16);
            }
            u8* dst = host.data() + by * row_bytes + static_cast<std::size_t>(bx) * BlockBytes;
            bc7enc_compress_block(dst, block, &params);
        }
    }
}

void Put(std::span<u8> host, std::size_t row_bytes, int size, const Tile& tile) {
    const int x0 = std::min(tile.x, tile.clear[0]);
    const int y0 = std::min(tile.y, tile.clear[1]);
    const int x1 = std::max(tile.x + tile.width, tile.clear[2]);
    const int y1 = std::max(tile.y + tile.height, tile.clear[3]);
    if (x0 < 0 || y0 < 0 || x1 > size || y1 > size ||
        tile.rgba.size() != static_cast<std::size_t>(tile.width) * tile.height * 4) {
        return;
    }
    Patch p = Decode(host, row_bytes, x0, y0, x1, y1, size);
    for (int y = tile.clear[1]; y < tile.clear[3]; ++y) {
        for (int x = tile.clear[0]; x < tile.clear[2]; ++x) {
            u8* px = p.At(x, y);
            px[3] = 0;
        }
    }
    for (int y = 0; y < tile.height; ++y) {
        for (int x = 0; x < tile.width; ++x) {
            const u8* s = tile.rgba.data() + (static_cast<std::size_t>(y) * tile.width + x) * 4;
            const int sa = s[3];
            if (sa == 0) {
                continue;
            }
            u8* d = p.At(tile.x + x, tile.y + y);
            const int da = d[3] * (255 - sa) / 255;
            const int out_a = sa + da;
            for (int c = 0; c < 3; ++c) {
                d[c] = static_cast<u8>((s[c] * sa + d[c] * da) / out_a);
            }
            d[3] = static_cast<u8>(out_a);
        }
    }
    // Fully clear pixels keep a dark colour, which BC7 encodes best next to the outlines.
    for (std::size_t i = 0; i < p.rgba.size(); i += 4) {
        if (p.rgba[i + 3] == 0) {
            p.rgba[i] = p.rgba[i + 1] = p.rgba[i + 2] = 51;
        }
    }
    Encode(p, host, row_bytes);
}

} // namespace

void SetPlan(std::vector<Tile> atlas, std::vector<Tile> title) {
    Plan& p = P();
    std::scoped_lock lock{p.mutex};
    p.atlas = std::move(atlas);
    p.title = std::move(title);
}

bool HasPlan() {
    Plan& p = P();
    std::scoped_lock lock{p.mutex};
    return !p.atlas.empty() || !p.title.empty();
}

bool Apply(u64 hash, u32 width, u32 height, std::span<u8> host, std::size_t row_bytes) {
    if ((hash != AtlasHash && hash != TitleHash) || width != TextureSize ||
        height != TextureSize) {
        return false;
    }
    if (row_bytes * (TextureSize / 4) > host.size()) {
        return false;
    }
    Plan& p = P();
    std::scoped_lock lock{p.mutex};
    const auto& tiles = hash == AtlasHash ? p.atlas : p.title;
    for (const Tile& tile : tiles) {
        Put(host, row_bytes, static_cast<int>(TextureSize), tile);
    }
    if (!tiles.empty()) {
        LOG_INFO(HW_GPU, "Pokopia button prompts: {} drawn from your controls",
                 hash == AtlasHash ? "atlas" : "title");
    }
    return !tiles.empty();
}

} // namespace VideoCommon::PokopiaGlyphs
