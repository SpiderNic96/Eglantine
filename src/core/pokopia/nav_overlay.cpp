// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/nav_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/host_overlay.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/pokopia/hooks.h"
#include "core/pokopia/mouse_camera.h"
#include "hid_core/hid_core.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

namespace Core::Pokopia {

namespace {

constexpr int W = static_cast<int>(Common::HostOverlay::Width);
constexpr int H = static_cast<int>(Common::HostOverlay::Height);
constexpr std::chrono::nanoseconds TickNs{16'666'667};

// Overlay pixels are half the game's 1920x1080 canvas.
constexpr float RadarX = 96.f, RadarY = 444.f, RadarR = 78.f;
constexpr float RadarRange = 4000.f; // world units at the radar's rim
constexpr float ArrowX = 480.f, ArrowY = 44.f;
constexpr float ArriveRange = 200.f; // world units: the ping is reached
constexpr float LiftRange = 300.f;   // G this close to the ping lifts it
constexpr float UnitsPerMetre = 100.f;

// The minimap panel (when the game's map texture is live), overlay pixels.
constexpr float PanelX0 = 16.f, PanelY0 = 352.f, PanelSize = 172.f;
constexpr float PanelCells = 26.f; // map cells across the panel

// Pokopia 2.0.1's minimap (main offsets). The pause menu's map is a 1024x1024 render target,
// "tex_minimap": main+0xf637c0 asks for a redraw when the menu opens; main+0x1047940 redraws it
// (only if asked and something changed), one sprite per world cell of 800 units.
constexpr u64 MinimapOwner = 0x3561780;     // global: an object whose +0x1200 is the map system
constexpr u64 MinimapSystem = 0x1200;
constexpr u64 MinimapFields = 0x63bd0;      // in the map system: texture, atlas, ...
constexpr u64 MinimapDirty = 0x20;          // u8: something changed since the last redraw
constexpr u64 MinimapRequest = 0x21;        // u8: redraw (if dirty); cleared by the redraw
constexpr u64 MinimapRenderSite = 0x1047c20; // str w8, [sp, #0x64]: w21/w27 first cell, w19 extent
constexpr u64 TextureNvnOffset = 0x58;      // texture object -> its NVNtexture
constexpr float CellUnits = 800.f;
constexpr float CellOrigin = 102400.f;      // world -102400 is cell 0
constexpr u32 TextureSize = 1024;
constexpr u32 MapBackground = 0xFFE4E4E4;    // under the map's see-through cells, as in the menu
constexpr u32 DefaultRefreshMs = 2000;      // ask for a redraw this often (if it changed)
constexpr s64 SearchNs = 600'000'000;      // after a redraw the renderer looks for the texture

struct Color {
    float r, g, b, a; // 0..1, straight alpha
};
constexpr Color Shade{0.05f, 0.06f, 0.09f, 0.55f};
constexpr Color Dark{0.05f, 0.05f, 0.07f, 0.9f};
constexpr Color White{1.f, 1.f, 1.f, 0.95f};
constexpr Color Player{0.98f, 0.80f, 0.93f, 1.f}; // the game's pink
constexpr Color Ping{1.f, 0.33f, 0.20f, 1.f};

struct Vec {
    float x, y;
};
Vec operator+(Vec a, Vec b) {
    return {a.x + b.x, a.y + b.y};
}
Vec operator-(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y};
}
Vec operator*(Vec a, float k) {
    return {a.x * k, a.y * k};
}
float Length(Vec a) {
    return std::hypot(a.x, a.y);
}
Vec Perp(Vec a) {
    return {-a.y, a.x};
}

// 5x7 glyphs, rows top to bottom, bit 4 is the left column.
std::optional<std::array<u8, 7>> Glyph(char c) {
    switch (c) {
    case '0': return std::array<u8, 7>{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return std::array<u8, 7>{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return std::array<u8, 7>{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return std::array<u8, 7>{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E};
    case '4': return std::array<u8, 7>{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return std::array<u8, 7>{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    case '6': return std::array<u8, 7>{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return std::array<u8, 7>{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return std::array<u8, 7>{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return std::array<u8, 7>{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    case 'm': return std::array<u8, 7>{0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11};
    case 'k': return std::array<u8, 7>{0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12};
    case '.': return std::array<u8, 7>{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case 'N': return std::array<u8, 7>{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'H': return std::array<u8, 7>{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'E': return std::array<u8, 7>{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'R': return std::array<u8, 7>{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case ' ': return std::array<u8, 7>{};
    default: return std::nullopt;
    }
}

class Canvas {
public:
    std::vector<u8> px = std::vector<u8>(Common::HostOverlay::Size);

    void Clear() {
        std::fill(px.begin(), px.end(), u8{0});
    }

    /// Premultiplied "over" with coverage.
    void Blend(int x, int y, Color c, float cover) {
        if (x < 0 || y < 0 || x >= W || y >= H) {
            return;
        }
        const float a = std::clamp(c.a * cover, 0.f, 1.f);
        if (a <= 0.f) {
            return;
        }
        u8* p = &px[(static_cast<std::size_t>(y) * W + x) * 4];
        const float k = 1.f - a;
        p[0] = static_cast<u8>(std::lround(c.r * 255.f * a + p[0] * k));
        p[1] = static_cast<u8>(std::lround(c.g * 255.f * a + p[1] * k));
        p[2] = static_cast<u8>(std::lround(c.b * 255.f * a + p[2] * k));
        p[3] = static_cast<u8>(std::lround(255.f * a + p[3] * k));
    }

    void Rect(float x0, float y0, float x1, float y1, Color color) {
        Shape(x0 - 1, y0 - 1, x1 + 1, y1 + 1, color, [&](float x, float y) {
            return std::min({x - x0, x1 - x, y - y0, y1 - y});
        });
    }

    /// A rectangle's outline, `width` wide, inside the rectangle.
    void Frame(float x0, float y0, float x1, float y1, float width, Color color) {
        Shape(x0 - 1, y0 - 1, x1 + 1, y1 + 1, color, [&](float x, float y) {
            const float inside = std::min({x - x0, x1 - x, y - y0, y1 - y});
            return std::min(inside, width - inside);
        });
    }

    void Disc(Vec c, float r, Color color) {
        Shape(c.x - r - 1, c.y - r - 1, c.x + r + 1, c.y + r + 1, color, [&](float x, float y) {
            return r - Length(Vec{x, y} - c);
        });
    }

    void Ring(Vec c, float r, float width, Color color) {
        const float o = r + width;
        Shape(c.x - o - 1, c.y - o - 1, c.x + o + 1, c.y + o + 1, color, [&](float x, float y) {
            return width * 0.5f - std::abs(Length(Vec{x, y} - c) - r);
        });
    }

    void Triangle(Vec a, Vec b, Vec c, Color color) {
        const auto cross = [](Vec u, Vec v) { return u.x * v.y - u.y * v.x; };
        const float sign = cross(b - a, c - a) < 0.f ? -1.f : 1.f;
        const std::array<Vec, 3> v{a, b, c};
        const float x0 = std::min({a.x, b.x, c.x}) - 1, x1 = std::max({a.x, b.x, c.x}) + 1;
        const float y0 = std::min({a.y, b.y, c.y}) - 1, y1 = std::max({a.y, b.y, c.y}) + 1;
        Shape(x0, y0, x1, y1, color, [&](float x, float y) {
            float inside = 1e9f;
            for (int i = 0; i < 3; ++i) {
                const Vec e = v[(i + 1) % 3] - v[i];
                const float len = std::max(Length(e), 1e-4f);
                inside = std::min(inside, sign * cross(e, Vec{x, y} - v[i]) / len);
            }
            return inside;
        });
    }

    /// Text of 5x7 glyphs, `scale` pixels per dot, centred on x, top at y, with a dark outline.
    void Text(std::string_view text, float cx, float y, int scale, Color color) {
        const int width = static_cast<int>(text.size()) * 6 * scale - scale;
        const int x0 = static_cast<int>(std::lround(cx)) - width / 2;
        const int y0 = static_cast<int>(std::lround(y));
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto glyph = Glyph(text[i]);
                if (!glyph) {
                    continue;
                }
                for (int row = 0; row < 7; ++row) {
                    for (int col = 0; col < 5; ++col) {
                        if (((*glyph)[row] >> (4 - col) & 1) == 0) {
                            continue;
                        }
                        const int gx = x0 + static_cast<int>(i) * 6 * scale + col * scale;
                        const int gy = y0 + row * scale;
                        const int grow = pass == 0 ? 1 : 0;
                        for (int yy = gy - grow; yy < gy + scale + grow; ++yy) {
                            for (int xx = gx - grow; xx < gx + scale + grow; ++xx) {
                                Blend(xx, yy, pass == 0 ? Dark : color, 1.f);
                            }
                        }
                    }
                }
            }
        }
    }

private:
    /// Fills where `inside(x, y)` (distance into the shape, in pixels) is positive, antialiased.
    template <typename F>
    void Shape(float fx0, float fy0, float fx1, float fy1, Color color, F&& inside) {
        const int x0 = std::max(0, static_cast<int>(std::floor(fx0)));
        const int y0 = std::max(0, static_cast<int>(std::floor(fy0)));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(fx1)));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(fy1)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float cover = std::clamp(inside(x + 0.5f, y + 0.5f) + 0.5f, 0.f, 1.f);
                if (cover > 0.f) {
                    Blend(x, y, color, cover);
                }
            }
        }
    }
};

/// A pointer: tip ahead along `dir`, `len` long and `half` wide at its base, with an outline.
void Pointer(Canvas& canvas, Vec at, Vec dir, float len, float half, Color color) {
    const Vec side = Perp(dir);
    const Vec tip = at + dir * (len * 0.6f);
    const Vec back = at - dir * (len * 0.4f);
    const float o = 2.5f;
    canvas.Triangle(tip + dir * (o * 1.6f), back - dir * o + side * (half + o * 1.6f),
                    back - dir * o - side * (half + o * 1.6f), Dark);
    canvas.Triangle(tip, back + side * half, back - side * half, color);
}

std::string Distance(float units) {
    const float metres = units / UnitsPerMetre;
    if (metres >= 1000.f) {
        const int tenths = static_cast<int>(std::lround(metres / 100.f));
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "km";
    }
    return std::to_string(static_cast<int>(std::lround(metres))) + "m";
}

Core::System* g_system = nullptr;
u64 g_main_base = 0;
std::shared_ptr<Core::Timing::EventType> g_tick_event;

// What the game's last minimap redraw covered (written by the hook, on a CPU core's thread).
struct MapInfo {
    bool drawn = false;
    s32 first_x = 0, first_y = 0; // the texture's first cell
    u32 cell_px = 16;             // texels per cell
    s32 extent = 0;               // the area's size in cells (the larger side)
    u64 fields = 0;               // the map system's MinimapFields
    u64 address = 0;              // the texture's device address, once found
    bool searched = false;
    std::chrono::steady_clock::time_point redrawn{};
};
std::mutex g_map_mutex;
MapInfo g_map;

bool Valid(Core::Memory::Memory& memory, u64 address, u64 size = 8) {
    return address != 0 && memory.IsValidVirtualAddressRange(address, size);
}

/// The texture's GPU address is somewhere in its NVNtexture (whose layout is the driver's): take
/// the first value there that maps a whole 1024x1024 RGBA8 image in the GPU's address space.
void FindTexture(Core::System& system, u64 fields) {
    auto& memory = system.ApplicationMemory();
    g_map.searched = true;
    if (!Valid(memory, fields)) {
        return;
    }
    const u64 texture = memory.Read64(fields);
    if (!Valid(memory, texture + TextureNvnOffset)) {
        return;
    }
    const u64 nvn = memory.Read64(texture + TextureNvnOffset);
    static const int pick = [] {
        const char* v = std::getenv("EDEN_POKOPIA_MAP_PICK");
        return v != nullptr ? std::atoi(v) : 0;
    }();
    int found = 0;
    auto& gpu_memory = system.GPU().Maxwell3D().GetMemoryManager();
    for (const auto& [base, what] : {std::pair{nvn, "nvn"}, std::pair{texture, "texture"}}) {
        if (!Valid(memory, base, 0x100)) {
            continue;
        }
        for (u64 offset = 0; offset < 0x100; offset += 8) {
            const u64 value = memory.Read64(base + offset);
            if (value == 0 || (value & 0xFFF) != 0 || value >= (u64{1} << 40)) {
                continue;
            }
            const auto device =
                gpu_memory.GpuToCpuAddress(value, std::size_t{TextureSize} * TextureSize * 4);
            if (!device) {
                continue;
            }
            LOG_INFO(Loader, "Pokopia minimap: candidate {} {} {}+{:#x}: gpu {:#x} -> device {:#x}",
                     found, what, base, offset, value, *device);
            if (found++ == pick) {
                g_map.address = *device;
            }
        }
    }
    if (g_map.address == 0) {
        LOG_INFO(Loader,
                 "Pokopia minimap: texture {:#x} (nvn {:#x}): no GPU address in it I know; the "
                 "renderer looks for the texture by its size",
                 texture, nvn);
        if (Valid(memory, nvn, 0xC0)) {
            for (u64 offset = 0; offset < 0xC0; offset += 0x20) {
                LOG_INFO(Loader, "Pokopia minimap: nvn+{:02x}: {:016x} {:016x} {:016x} {:016x}",
                         offset, memory.Read64(nvn + offset), memory.Read64(nvn + offset + 8),
                         memory.Read64(nvn + offset + 16), memory.Read64(nvn + offset + 24));
            }
        }
    } else {
        LOG_INFO(Loader, "Pokopia minimap: texture at device {:#x}", g_map.address);
    }
}

void OnMinimapRender(Core::System& system, Kernel::Svc::ThreadContext& ctx) {
    std::scoped_lock lock{g_map_mutex};
    const s32 first_x = static_cast<s32>(ctx.r[21]);
    const s32 first_y = static_cast<s32>(ctx.r[27]);
    const u32 cell_px = static_cast<s32>(ctx.r[19]) < 64 ? 16 : 8;
    if (!g_map.drawn || first_x != g_map.first_x || first_y != g_map.first_y ||
        cell_px != g_map.cell_px) {
        LOG_INFO(Loader, "Pokopia minimap: redrawn, first cell {} {}, {} texels a cell", first_x,
                 first_y, cell_px);
    }
    g_map.first_x = first_x;
    g_map.first_y = first_y;
    g_map.cell_px = cell_px;
    g_map.extent = static_cast<s32>(ctx.r[19]);
    g_map.fields = ctx.r[24];
    g_map.drawn = true;
    g_map.redrawn = std::chrono::steady_clock::now();
    if (!g_map.searched) {
        FindTexture(system, g_map.fields);
    }
}

/// Asks the game to redraw its minimap (it does only if something changed); the first time also
/// marks it changed, for the first picture.
void RequestRedraw(Core::System& system, bool mark_changed, bool first) {
    auto& memory = system.ApplicationMemory();
    if (!Valid(memory, g_main_base + MinimapOwner)) {
        return;
    }
    const u64 owner = memory.Read64(g_main_base + MinimapOwner);
    if (!Valid(memory, owner + MinimapSystem)) {
        return;
    }
    const u64 system_object = memory.Read64(owner + MinimapSystem);
    const u64 fields = system_object + MinimapFields;
    if (system_object == 0 || !Valid(memory, fields, 0x28)) {
        return;
    }
    if (memory.Read64(fields) == 0 || memory.Read64(fields + 8) == 0) {
        return; // no texture yet
    }
    if (first) {
        LOG_INFO(Loader, "Pokopia minimap: map fields at {:#x}, asking for redraws", fields);
    }
    if (mark_changed) {
        memory.Write8(fields + MinimapDirty, 1);
    }
    memory.Write8(fields + MinimapRequest, 1);
}

struct State {
    Canvas canvas;
    bool shown = false;
    bool ping = false;
    float ping_x = 0.f, ping_y = 0.f, ping_z = 0.f;
    u64 last_signature = ~u64{0};
    u32 frame = 0;
    bool requested_once = false;
    bool mark_changed = true; // back in play: have the map redrawn even if nothing changed
};

State& St() {
    static State state;
    return state;
}

/// Where the map panel shows the world: texel crop and a world -> panel mapping.
struct PanelView {
    bool live = false;
    float crop_x = 0.f, crop_y = 0.f, crop_size = 1.f; // texels
    s32 first_x = 0, first_y = 0;
    float cell_px = 16.f;
    Vec ToPanel(float x, float z) const {
        const float tx = ((x + CellOrigin) / CellUnits - first_x) * cell_px;
        const float ty = ((z + CellOrigin) / CellUnits - first_y) * cell_px;
        return {PanelX0 + (tx - crop_x) / crop_size * PanelSize,
                PanelY0 + (ty - crop_y) / crop_size * PanelSize};
    }
};

void DrawPing(Canvas& c, Vec at, Vec centre, float limit_x0, float limit_y0, float limit_x1,
              float limit_y1) {
    if (at.x >= limit_x0 && at.x <= limit_x1 && at.y >= limit_y0 && at.y <= limit_y1) {
        c.Disc(at, 6.5f, Dark);
        c.Disc(at, 5.f, Ping);
        return;
    }
    // Off the panel: a pointer on its edge, towards the ping.
    Vec dir = at - centre;
    dir = dir * (1.f / std::max(Length(dir), 1e-4f));
    const float tx = dir.x > 0 ? (limit_x1 - centre.x) / dir.x
                               : dir.x < 0 ? (limit_x0 - centre.x) / dir.x : 1e9f;
    const float ty = dir.y > 0 ? (limit_y1 - centre.y) / dir.y
                               : dir.y < 0 ? (limit_y0 - centre.y) / dir.y : 1e9f;
    Pointer(c, centre + dir * std::min(tx, ty), dir, 13.f, 5.f, Ping);
}

void Draw(State& s, const CameraInfo& cam, Vec forward, const PanelView& view) {
    Canvas& c = s.canvas;
    c.Clear();
    const Vec player{cam.target[0], cam.target[2]};

    if (view.live) {
        // The game's map is under this (a layer of its own): a frame, north, the player, the ping.
        const float x1 = PanelX0 + PanelSize, y1 = PanelY0 + PanelSize;
        c.Frame(PanelX0 - 3.f, PanelY0 - 3.f, x1 + 3.f, y1 + 3.f, 3.f, White);
        c.Frame(PanelX0 - 5.f, PanelY0 - 5.f, x1 + 5.f, y1 + 5.f, 2.f, Dark);
        c.Text("N", PanelX0 + PanelSize * 0.5f, PanelY0 + 4.f, 2, White);
        const Vec at = view.ToPanel(player.x, player.y);
        if (s.ping) {
            DrawPing(c, view.ToPanel(s.ping_x, s.ping_z), at, PanelX0 + 6.f, PanelY0 + 6.f,
                     x1 - 6.f, y1 - 6.f);
        }
        Pointer(c, at, forward, 18.f, 7.f, Player);
    } else {
        // Radar, north up as the game's map: world +x right, +z down.
        const Vec centre{RadarX, RadarY};
        c.Disc(centre, RadarR, Shade);
        c.Ring(centre, RadarR * 0.5f, 1.f, Color{1.f, 1.f, 1.f, 0.25f});
        c.Ring(centre, RadarR, 2.f, White);
        c.Text("N", RadarX, RadarY - RadarR + 5.f, 2, White);
        if (s.ping) {
            const Vec rel = (Vec{s.ping_x, s.ping_z} - player) * (RadarR / RadarRange);
            const float len = Length(rel);
            if (len > RadarR - 8.f) {
                const Vec dir = rel * (1.f / len);
                Pointer(c, centre + dir * (RadarR - 8.f), dir, 13.f, 5.f, Ping);
            } else {
                c.Disc(centre + rel, 6.5f, Dark);
                c.Disc(centre + rel, 5.f, Ping);
            }
        }
        Pointer(c, centre, forward, 18.f, 7.f, Player);
    }

    if (!s.ping) {
        return;
    }
    // The guide arrow: up is where the camera looks.
    const Vec to_ping = Vec{s.ping_x, s.ping_z} - player;
    const float dist = Length(to_ping);
    const Vec arrow{ArrowX, ArrowY};
    if (dist < ArriveRange) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(s.frame) * 0.15f);
        c.Ring(arrow, 14.f + pulse * 4.f, 4.f, Dark);
        c.Ring(arrow, 14.f + pulse * 4.f, 2.5f, Ping);
        c.Text("HERE", ArrowX, ArrowY + 26.f, 2, White);
        return;
    }
    const Vec right{-forward.y, forward.x};
    const float ahead = to_ping.x * forward.x + to_ping.y * forward.y;
    const float across = to_ping.x * right.x + to_ping.y * right.y;
    Vec screen{across, -ahead};
    screen = screen * (1.f / std::max(Length(screen), 1e-4f));
    Pointer(c, arrow, screen, 34.f, 13.f, Ping);
    c.Text(Distance(dist), ArrowX, ArrowY + 26.f, 2, White);
}

PanelView MapView(const CameraInfo& cam) {
    PanelView view{};
    MapInfo map;
    {
        std::scoped_lock lock{g_map_mutex};
        map = g_map;
    }
    if (!map.drawn) {
        return view;
    }
    view.live = true;
    view.first_x = map.first_x;
    view.first_y = map.first_y;
    view.cell_px = static_cast<float>(map.cell_px);
    view.crop_size = std::min(PanelCells * view.cell_px, static_cast<float>(TextureSize));
    const float tx = ((cam.target[0] + CellOrigin) / CellUnits - map.first_x) * view.cell_px;
    const float ty = ((cam.target[2] + CellOrigin) / CellUnits - map.first_y) * view.cell_px;
    const float max = static_cast<float>(TextureSize) - view.crop_size;
    view.crop_x = std::round(std::clamp(tx - view.crop_size * 0.5f, 0.f, max));
    view.crop_y = std::round(std::clamp(ty - view.crop_size * 0.5f, 0.f, max));

    static const bool flip = [] {
        const char* v = std::getenv("EDEN_POKOPIA_MAP_FLIP");
        return v != nullptr && v[0] == '1';
    }();
    Common::HostOverlay::GuestTexture texture{};
    texture.visible = true;
    texture.address = map.address;
    texture.search = std::chrono::steady_clock::now() - map.redrawn < std::chrono::nanoseconds{SearchNs};
    texture.width = TextureSize;
    texture.height = TextureSize;
    texture.crop[0] = view.crop_x;
    texture.crop[1] = flip ? view.crop_y + view.crop_size : view.crop_y;
    texture.crop[2] = view.crop_x + view.crop_size;
    texture.crop[3] = flip ? view.crop_y : view.crop_y + view.crop_size;
    texture.dest[0] = PanelX0 / W;
    texture.dest[1] = PanelY0 / H;
    texture.dest[2] = (PanelX0 + PanelSize) / W;
    texture.dest[3] = (PanelY0 + PanelSize) / H;
    texture.underlay = MapBackground;
    Common::GetHostOverlay().SetGuestTexture(texture);
    return view;
}

Common::HostOverlay::GuestTexture HiddenTexture() {
    // Keeps its size: a change of size rebuilds the renderer's copy of the map.
    Common::HostOverlay::GuestTexture texture{};
    texture.width = TextureSize;
    texture.height = TextureSize;
    return texture;
}

void Hide(State& s) {
    Common::GetHostOverlay().SetGuestTexture(HiddenTexture());
    s.mark_changed = true;
    if (s.shown) {
        s.canvas.Clear();
        Common::GetHostOverlay().Publish(s.canvas.px);
        s.shown = false;
        s.last_signature = ~u64{0};
    }
}

void Tick() {
    State& s = St();
    auto& hid = g_system->HIDCore();
    auto& overlay = Common::GetHostOverlay();
    ++s.frame;

    CameraInfo cam{};
    const bool have_camera = CameraState(*g_system, &cam);
    if (hid.TakeNavPing() && have_camera) {
        const float dx = cam.target[0] - s.ping_x, dz = cam.target[2] - s.ping_z;
        if (s.ping && std::hypot(dx, dz) < LiftRange) {
            s.ping = false;
            LOG_INFO(Loader, "Pokopia navigation: ping lifted");
        } else {
            s.ping = true;
            s.ping_x = cam.target[0];
            s.ping_y = cam.target[1];
            s.ping_z = cam.target[2];
            LOG_INFO(Loader, "Pokopia navigation: ping at {:.0f} {:.0f} {:.0f}", s.ping_x, s.ping_y,
                     s.ping_z);
        }
        s.last_signature = ~u64{0};
    }

    if (!have_camera || !hid.IsInPlay()) {
        Hide(s);
        return;
    }
    if (!overlay.Enabled()) {
        overlay.SetEnabled(true);
    }
    static const u32 refresh_ticks = [] {
        const char* v = std::getenv("EDEN_POKOPIA_MAP_REFRESH_MS");
        const long ms = v != nullptr ? std::strtol(v, nullptr, 10) : DefaultRefreshMs;
        return static_cast<u32>(std::max<long>(1, ms * 60 / 1000));
    }();
    if (s.mark_changed || s.frame % refresh_ticks == 0) {
        RequestRedraw(*g_system, s.mark_changed, !s.requested_once);
        s.requested_once = true;
        s.mark_changed = false;
    }

    Vec forward{cam.target[0] - cam.eye[0], cam.target[2] - cam.eye[2]};
    const float flen = Length(forward);
    forward = flen > 1e-3f ? forward * (1.f / flen) : Vec{std::sin(cam.yaw), std::cos(cam.yaw)};
    const PanelView view = MapView(cam);

    // Redraw only when something on it moved (the arrival pulse always moves).
    const auto q = [](float v, float step) { return static_cast<u64>(static_cast<s64>(v / step)); };
    u64 signature = q(cam.target[0], 8.f) * 1000003ULL ^ q(cam.target[2], 8.f) * 998244353ULL ^
                    q(std::atan2(forward.y, forward.x), 0.01f) * 7919ULL ^
                    (s.ping ? q(s.ping_x, 1.f) * 31ULL ^ q(s.ping_z, 1.f) * 131ULL ^ 1ULL : 0ULL) ^
                    (view.live ? q(view.crop_x, 1.f) * 65537ULL ^ q(view.crop_y, 1.f) * 257ULL ^ 2ULL
                               : 0ULL);
    const bool arrived = s.ping && std::hypot(s.ping_x - cam.target[0],
                                              s.ping_z - cam.target[2]) < ArriveRange;
    if (signature == s.last_signature && !arrived && s.shown) {
        return;
    }
    s.last_signature = signature;
    Draw(s, cam, forward, view);
    overlay.Publish(s.canvas.px);
    s.shown = true;
}

} // namespace

bool PingFromMenuMap(float u, float v) {
    MapInfo map;
    {
        std::scoped_lock lock{g_map_mutex};
        map = g_map;
    }
    if (!map.drawn || map.extent <= 0 || u < 0.f || u > 1.f || v < 0.f || v > 1.f) {
        return false;
    }
    // The menu shows the area around the texture's centre: its extent in cells, plus a margin
    // (main+0xf63b40 sets the map's UVs: 0.5 -+ extent * 800 * 1.0535 / 2 / the texture's world size).
    constexpr float MenuMargin = 1.0534979f;
    const float span =
        static_cast<float>(map.extent) * MenuMargin * static_cast<float>(map.cell_px);
    const float tx = TextureSize * 0.5f + (u - 0.5f) * span;
    const float ty = TextureSize * 0.5f + (v - 0.5f) * span;
    State& s = St();
    s.ping = true;
    s.ping_x = (tx / map.cell_px + map.first_x) * CellUnits - CellOrigin;
    s.ping_z = (ty / map.cell_px + map.first_y) * CellUnits - CellOrigin;
    CameraInfo cam{};
    s.ping_y = CameraState(*g_system, &cam) ? cam.target[1] : 0.f;
    s.last_signature = ~u64{0};
    LOG_INFO(Loader, "Pokopia navigation: ping from the menu's map ({:.2f}, {:.2f}) at {:.0f} {:.0f}",
             u, v, s.ping_x, s.ping_z);
    return true;
}

void InstallNavOverlay(Core::System& system, HookInstaller& hooks, u64 main_base) {
    g_system = &system;
    g_main_base = main_base;
    {
        std::scoped_lock lock{g_map_mutex};
        g_map = MapInfo{};
    }
    State& s = St();
    s.shown = false;
    s.ping = false;
    s.requested_once = false;
    s.mark_changed = true;
    s.last_signature = ~u64{0};
    s.canvas.Clear();
    Common::GetHostOverlay().Publish(s.canvas.px);
    Common::GetHostOverlay().SetGuestTexture(HiddenTexture());

    if (!hooks.AddInline(MinimapRenderSite, &OnMinimapRender, "minimap redraw")) {
        LOG_WARNING(Loader, "Pokopia navigation overlay: no live map (radar only)");
    }

    auto& timing = system.CoreTiming();
    if (g_tick_event) {
        timing.UnscheduleEvent(g_tick_event);
    } else {
        g_tick_event = Core::Timing::CreateEvent(
            "Pokopia navigation overlay",
            [](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                if (g_system != nullptr) {
                    Tick();
                }
                return std::nullopt;
            });
    }
    timing.ScheduleLoopingEvent(TickNs, TickNs, g_tick_event);
    LOG_INFO(Loader, "Pokopia navigation overlay: on (G drops or lifts a ping)");
}

} // namespace Core::Pokopia
