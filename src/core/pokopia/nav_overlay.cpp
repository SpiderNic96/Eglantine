// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/nav_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <utility>
#include <unordered_map>
#include <bit>
#include <mutex>
#include <chrono>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/host_overlay.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/pokopia/hooks.h"
#include "core/pokopia/mouse_camera.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
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
    case 'P': return std::array<u8, 7>{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'I': return std::array<u8, 7>{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case 'G': return std::array<u8, 7>{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
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
    void Text(std::string_view text, float cx, float y, int scale, Color color,
              bool outline = true) {
        const int width = static_cast<int>(text.size()) * 6 * scale - scale;
        const int x0 = static_cast<int>(std::lround(cx)) - width / 2;
        const int y0 = static_cast<int>(std::lround(y));
        for (int pass = outline ? 0 : 1; pass < 2; ++pass) {
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

    /// Fills a box with `cover(x, y)` (0..1) of `color`.
    template <typename F>
    void Fill(float fx0, float fy0, float fx1, float fy1, Color color, F&& cover) {
        const int x0 = std::max(0, static_cast<int>(std::floor(fx0)));
        const int y0 = std::max(0, static_cast<int>(std::floor(fy0)));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(fx1)));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(fy1)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float k = cover(x + 0.5f, y + 0.5f);
                if (k > 0.f) {
                    Blend(x, y, color, k);
                }
            }
        }
    }

    /// Any simple polygon, antialiased: inside by the even-odd rule, edges by distance.
    void Polygon(std::span<const Vec> points, Color color) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const Vec& p : points) {
            x0 = std::min(x0, p.x), y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
        }
        Shape(x0 - 1, y0 - 1, x1 + 1, y1 + 1, color, [&](float x, float y) {
            bool in = false;
            float edge = 1e9f;
            for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                const Vec a = points[j], b = points[i];
                if ((b.y > y) != (a.y > y) && x < (a.x - b.x) * (y - b.y) / (a.y - b.y) + b.x) {
                    in = !in;
                }
                const Vec ab = b - a, ap = Vec{x, y} - a;
                const float t =
                    std::clamp((ap.x * ab.x + ap.y * ab.y) / std::max(ab.x * ab.x + ab.y * ab.y,
                                                                      1e-6f),
                               0.f, 1.f);
                edge = std::min(edge, Length(ap - ab * t));
            }
            return in ? edge : -edge;
        });
    }

    void RoundRect(float x0, float y0, float x1, float y1, float r, Color color) {
        Shape(x0 - 1, y0 - 1, x1 + 1, y1 + 1, color, [&](float x, float y) {
            const float dx = std::max({x0 + r - x, 0.f, x - (x1 - r)});
            const float dy = std::max({y0 + r - y, 0.f, y - (y1 - r)});
            if (dx > 0.f && dy > 0.f) {
                return r - std::hypot(dx, dy);
            }
            return std::min({x - x0, x1 - x, y - y0, y1 - y});
        });
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
    const float o = 2.f;
    canvas.Triangle(tip + dir * (o * 1.6f), back - dir * o + side * (half + o * 1.6f),
                    back - dir * o - side * (half + o * 1.6f), White);
    canvas.Triangle(tip, back + side * half, back - side * half, color);
}

constexpr Color PlayerRed{0.92f, 0.18f, 0.22f, 1.f};

/// The player as on the game's map: a soft white view cone where the camera looks, a pink blob
/// and a red tip in front of it.
void PlayerMarker(Canvas& c, Vec at, Vec dir) {
    constexpr float ConeR = 30.f, ConeHalf = 0.6f;
    c.Fill(at.x - ConeR, at.y - ConeR, at.x + ConeR, at.y + ConeR, Color{1.f, 1.f, 1.f, 0.6f},
           [&](float x, float y) {
               const Vec d{x - at.x, y - at.y};
               const float len = Length(d);
               if (len < 1.f || len > ConeR) {
                   return 0.f;
               }
               const float angle =
                   std::acos(std::clamp((d.x * dir.x + d.y * dir.y) / len, -1.f, 1.f));
               const float edge = std::clamp((ConeHalf - angle) * len + 0.5f, 0.f, 1.f);
               return edge * (1.f - len / ConeR);
           });
    const Vec side = Perp(dir);
    const Vec tip = at + dir * 12.5f;
    c.Triangle(tip + dir * 2.f, at + dir * 4.f + side * 6.5f, at + dir * 4.f - side * 6.5f, White);
    c.Triangle(tip, at + dir * 5.5f + side * 4.5f, at + dir * 5.5f - side * 4.5f, PlayerRed);
    c.Disc(at + Vec{0.f, 1.f}, 7.5f, Color{0.f, 0.f, 0.f, 0.25f});
    c.Disc(at, 7.5f, White);
    c.Disc(at, 6.f, Player);
}

/// The guide arrow: a bold arrow with a white rim and a soft shadow, in the ping's colour.
void GuideArrow(Canvas& c, Vec at, Vec dir, Color color) {
    const Vec side = Perp(dir);
    const auto arrow = [&](Vec o, float grow, Color fill) {
        const Vec base = at + o;
        const float head = 16.f + grow * 1.6f, hw = 6.5f + grow;
        const std::array<Vec, 7> points{
            base + dir * (19.f + grow * 1.8f),
            base + dir * (2.f - grow) + side * head,
            base + dir * (2.f - grow) + side * hw,
            base - dir * (15.f + grow) + side * hw,
            base - dir * (15.f + grow) - side * hw,
            base + dir * (2.f - grow) - side * hw,
            base + dir * (2.f - grow) - side * head,
        };
        c.Polygon(points, fill);
    };
    arrow(Vec{0.f, 3.f}, 3.f, Color{0.f, 0.f, 0.f, 0.3f});
    arrow(Vec{}, 3.f, White);
    arrow(Vec{}, 0.f, color);
}

/// A label in a white rounded pill, centred on x, top at y.
void Pill(Canvas& c, std::string_view text, float cx, float y) {
    const float width = static_cast<float>(text.size()) * 12.f + 14.f;
    c.RoundRect(cx - width * 0.5f, y + 1.5f, cx + width * 0.5f, y + 25.5f, 12.f,
                Color{0.f, 0.f, 0.f, 0.25f});
    c.RoundRect(cx - width * 0.5f, y, cx + width * 0.5f, y + 24.f, 12.f,
                Color{1.f, 1.f, 1.f, 0.95f});
    c.Text(text, cx, y + 5.f, 2, Color{0.20f, 0.20f, 0.24f, 1.f}, false);
}

/// A Pokémon: a little Poké Ball.
void PokemonMarker(Canvas& c, Vec p) {
    constexpr float R = 4.5f;
    c.Disc(p, R + 1.3f, Dark);
    c.Disc(p, R, PlayerRed);
    c.Fill(p.x - R, p.y, p.x + R, p.y + R, White, [&](float x, float y) {
        return std::clamp(R + 0.5f - Length(Vec{x, y} - p), 0.f, 1.f) *
               std::clamp(y - p.y, 0.f, 1.f);
    });
    c.Rect(p.x - R, p.y - 0.6f, p.x + R, p.y + 0.6f, Dark);
    c.Disc(p, 1.6f, Dark);
    c.Disc(p, 0.9f, White);
}

/// A point of interest: a rounded badge in its kind's colour.
void PlaceMarker(Canvas& c, Vec p, Color color) {
    c.RoundRect(p.x - 6.f, p.y - 5.f, p.x + 6.f, p.y + 7.f, 3.5f, Color{0.f, 0.f, 0.f, 0.3f});
    c.RoundRect(p.x - 6.f, p.y - 6.f, p.x + 6.f, p.y + 6.f, 3.5f, White);
    c.RoundRect(p.x - 4.3f, p.y - 4.3f, p.x + 4.3f, p.y + 4.3f, 2.2f, color);
}

// Facilities by the menu's icon id (seen: 0x14c the way to another area, 0x14f a PC, 0x150 a
// lit building; 0x14d and 0x14e the house and the Pokémon Center).
enum class Place : u8 { Generic, Exit, Pc, Light, Center, House };

Place PlaceOf(u16 id) {
    switch (id) {
    case 0x14c: return Place::Exit;
    case 0x14d: return Place::Center;
    case 0x14e: return Place::House;
    case 0x14f: return Place::Pc;
    case 0x150: return Place::Light;
    default: return Place::Generic;
    }
}

/// A facility drawn like the game's map icons: a white-rimmed badge with a simple picture.
void FacilityMarker(Canvas& c, Vec p, Place place, Color fallback) {
    constexpr Color Ink{0.22f, 0.22f, 0.26f, 1.f};
    constexpr Color Green{0.30f, 0.72f, 0.45f, 1.f};
    constexpr Color Screen{0.85f, 0.97f, 0.90f, 1.f};
    const auto badge = [&](Color fill) {
        c.RoundRect(p.x - 8.f, p.y - 6.5f, p.x + 8.f, p.y + 8.5f, 4.f, Color{0.f, 0.f, 0.f, 0.3f});
        c.RoundRect(p.x - 8.f, p.y - 8.f, p.x + 8.f, p.y + 8.f, 4.f, White);
        c.RoundRect(p.x - 6.5f, p.y - 6.5f, p.x + 6.5f, p.y + 6.5f, 3.f, fill);
    };
    switch (place) {
    case Place::Exit: // two white chevrons on green
        badge(Green);
        for (const float dx : {-3.5f, 1.5f}) {
            c.Triangle(Vec{p.x + dx - 1.5f, p.y}, Vec{p.x + dx + 3.f, p.y - 4.f},
                       Vec{p.x + dx + 3.f, p.y + 4.f}, White);
        }
        break;
    case Place::Pc: // a green monitor with a pale screen
        badge(Green);
        c.RoundRect(p.x - 4.5f, p.y - 4.f, p.x + 4.5f, p.y + 2.5f, 1.2f, Screen);
        c.Rect(p.x - 2.5f, p.y + 3.2f, p.x + 2.5f, p.y + 4.6f, White);
        c.Disc(Vec{p.x, p.y - 0.8f}, 1.4f, Green);
        break;
    case Place::Light: // a warm lamp
        badge(Ink);
        c.Disc(Vec{p.x, p.y - 0.5f}, 3.6f, Color{1.f, 0.82f, 0.25f, 1.f});
        c.Disc(Vec{p.x, p.y - 1.f}, 1.6f, Color{1.f, 0.97f, 0.75f, 1.f});
        c.Rect(p.x - 2.f, p.y + 3.3f, p.x + 2.f, p.y + 5.f, Color{0.75f, 0.75f, 0.8f, 1.f});
        break;
    case Place::Center: // red roof over a Poké Ball door
        badge(Color{0.98f, 0.96f, 0.94f, 1.f});
        c.Triangle(Vec{p.x, p.y - 6.f}, Vec{p.x - 6.f, p.y - 0.5f}, Vec{p.x + 6.f, p.y - 0.5f},
                   PlayerRed);
        c.Rect(p.x - 4.5f, p.y - 0.5f, p.x + 4.5f, p.y + 5.f, Color{0.9f, 0.9f, 0.92f, 1.f});
        c.Disc(Vec{p.x, p.y + 2.f}, 2.2f, PlayerRed);
        c.Rect(p.x - 2.2f, p.y + 1.7f, p.x + 2.2f, p.y + 2.3f, Ink);
        break;
    case Place::House: // a pink house
        badge(Color{0.98f, 0.96f, 0.94f, 1.f});
        c.Triangle(Vec{p.x, p.y - 6.f}, Vec{p.x - 6.f, p.y - 0.5f}, Vec{p.x + 6.f, p.y - 0.5f},
                   Color{0.95f, 0.45f, 0.65f, 1.f});
        c.Rect(p.x - 4.f, p.y - 0.5f, p.x + 4.f, p.y + 5.f, Color{0.98f, 0.75f, 0.85f, 1.f});
        c.Rect(p.x - 1.3f, p.y + 1.5f, p.x + 1.3f, p.y + 5.f, Color{0.45f, 0.30f, 0.45f, 1.f});
        break;
    case Place::Generic:
        PlaceMarker(c, p, fallback);
        break;
    }
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

// The pause menu's map: where menu_mouse last saw it (1920x1080 layout units), and when.
struct MenuMapRect {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    std::chrono::steady_clock::time_point seen{};
};
std::mutex g_menu_map_mutex;
MenuMapRect g_menu_map;
constexpr auto MenuMapGone = std::chrono::milliseconds{200};
// The menu shows the area around the texture's centre: its extent in cells, plus a margin
// (main+0xf63b40 sets the map's UVs: 0.5 -+ extent * 800 * 1.0535 / 2 / the texture's world size).
constexpr float MenuMargin = 1.0534979f;

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

void ClearIcons();

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
    if (g_map.drawn && (first_x != g_map.first_x || first_y != g_map.first_y)) {
        ClearIcons(); // another area
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

// ---- map icons ----
// The pause menu's map places an icon per Pokémon and point of interest; each placing function
// turns the thing's world x and z (in float registers) into the map's coordinates. Hooks there
// keep what the menu showed. Pokémon also give their position's address (their component's
// position, which the game keeps updating), so they move on the minimap live afterwards.
struct IconSite {
    u64 site;      // ldp s2, s3, [xN, #0xa8]: the map's centre, about to be subtracted
    u8 x_reg, z_reg;
    u8 id_reg;     // the icon's id in a w register there, or NoIdReg
};
constexpr u8 NoIdReg = 0xFF;
constexpr std::array<IconSite, 6> IconSites{{
    {0xf64034, 9, 10, NoIdReg},
    {0xf65174, 13, 14, NoIdReg},
    {0xf65b40, 8, 9, NoIdReg},
    {0xf65ec8, 9, 10, NoIdReg},
    {0xf668c8, 9, 10, 20}, // main+0xf667f4: facilities, their icon id the third argument
    {0xf66e58, 12, 13, NoIdReg},
}};
// ldp x21, x20, [x0] after a Pokémon's position getter: x0 is the position, [sp + 0xc] its id,
// w19 its kind. One instruction before (ldr x8, [x8, #0x10]) x0 is the component the getter
// belongs to (diagnostics: for following Pokémon without the menu).
constexpr std::array<u64, 2> PokemonSites{0xf65798, 0xf65a58};
constexpr std::array<u64, 2> PokemonComponentSites{0xf65790, 0xf65a50};
// The area's list of Pokémon ids the menu goes through: [[main+0x3483bd0]] +0x2b8 (u16 ids),
// count +0x2c4.
constexpr u64 PokemonListGlobal = 0x3483bd0;
// A Pokémon's object (an id 0x2nnn): [list + 0x50 + nnn * 8], of this class. Its position is in
// the world's table of transforms: [[main+0x3483b60]] + 0x835af0 + slot * 0x30 + 0x10, slot the
// object's u32 at +0x382c (below 0x41a); main+0x666b68, the class's position getter.
constexpr u64 PokemonVtable = 0x322f7f8;
constexpr u64 WorldGlobal = 0x3483b60;
constexpr u64 TransformTable = 0x835af0;
constexpr u64 TransformSlot = 0x382c;
constexpr u32 TransformSlots = 0x41a;
constexpr std::array<Color, IconSites.size()> IconColors{{
    {0.90f, 0.20f, 0.20f, 1.f}, // red
    {0.20f, 0.45f, 0.95f, 1.f}, // blue
    {1.00f, 0.55f, 0.10f, 1.f}, // orange
    {0.65f, 0.30f, 0.90f, 1.f}, // purple
    {0.10f, 0.75f, 0.85f, 1.f}, // cyan
    {0.20f, 0.75f, 0.25f, 1.f}, // green
}};
// Facilities by icon id (main+0xf64700..0xf64a74 pass 0x14d..0x150; others come from data).
constexpr std::array<Color, 6> FacilityColors{{
    {0.10f, 0.75f, 0.85f, 1.f}, // cyan
    {0.95f, 0.35f, 0.30f, 1.f}, // red
    {0.98f, 0.70f, 0.15f, 1.f}, // amber
    {0.45f, 0.40f, 0.95f, 1.f}, // indigo
    {0.30f, 0.80f, 0.40f, 1.f}, // green
    {0.90f, 0.40f, 0.80f, 1.f}, // pink
}};

struct Icon {
    float x, z;
    u8 kind;
    u16 id;
    u32 pass;
};
struct TrackedPokemon {
    u64 position;
    u32 species;
    u32 pass;
};
std::mutex g_icon_mutex;
std::vector<Icon> g_icons;
std::unordered_map<u16, TrackedPokemon> g_pokemon;
u32 g_pass = 0; // one per time the menu placed its icons
std::chrono::steady_clock::time_point g_last_icon{}, g_pass_start{};
std::array<u32, IconSites.size() + 1> g_icon_logged{};
std::vector<std::pair<u8, u16>> g_logged_ids;
thread_local u64 t_component = 0;
u32 g_component_logged = 0;

/// An icon was placed: a new pass when the last one was a while ago (the menu opened again).
void TouchPass() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_icon > std::chrono::seconds(1)) {
        ++g_pass;
        g_pass_start = now;
    }
    g_last_icon = now;
}

float FloatReg(const Kernel::Svc::ThreadContext& ctx, u8 reg) {
    return std::bit_cast<float>(static_cast<u32>(ctx.v[reg][0]));
}

template <std::size_t I>
void OnIconPlaced(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    const float x = FloatReg(ctx, IconSites[I].x_reg);
    const float z = FloatReg(ctx, IconSites[I].z_reg);
    if (!std::isfinite(x) || !std::isfinite(z)) {
        return;
    }
    const u16 id = IconSites[I].id_reg == NoIdReg
                       ? 0xFFFF
                       : static_cast<u16>(ctx.r[IconSites[I].id_reg]);
    std::scoped_lock lock{g_icon_mutex};
    TouchPass();
    const std::pair<u8, u16> key{static_cast<u8>(I), id};
    if (g_logged_ids.size() < 64 && std::ranges::find(g_logged_ids, key) == g_logged_ids.end()) {
        g_logged_ids.push_back(key);
        LOG_INFO(Loader, "Pokopia minimap: icon kind {} id {:#x} at {:.0f} {:.0f}", I, id, x, z);
    }
    for (Icon& icon : g_icons) {
        if (icon.kind == I && std::abs(icon.x - x) < 1.f && std::abs(icon.z - z) < 1.f) {
            icon.id = id;
            icon.pass = g_pass;
            return;
        }
    }
    if (g_icons.size() < 1024) {
        g_icons.push_back({x, z, static_cast<u8>(I), id, g_pass});
    }
}

void OnPokemonComponent(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    t_component = ctx.r[0];
}

void OnPokemonPlaced(Core::System& system, Kernel::Svc::ThreadContext& ctx) {
    auto& memory = system.ApplicationMemory();
    const u16 id = memory.Read16(ctx.sp + 0xc);
    const u32 species = static_cast<u32>(ctx.r[19]);
    const u64 component = t_component;
    std::scoped_lock lock{g_icon_mutex};
    TouchPass();
    if (g_icon_logged.back()++ < 3) {
        LOG_INFO(Loader, "Pokopia minimap: Pokémon {} (kind {:#x}) with its position at {:#x}",
                 id, species, ctx.r[0]);
    }
    if (g_component_logged < 4 && Valid(memory, component)) {
        // How a Pokémon's position follows from its component: for following them live without
        // the menu (the getter is component's virtual function at +0x10).
        ++g_component_logged;
        const u64 vtable = memory.Read64(component);
        const u64 getter = Valid(memory, vtable + 0x10) ? memory.Read64(vtable + 0x10) : 0;
        u32 code[4]{};
        if (Valid(memory, getter, 16)) {
            for (int i = 0; i < 4; ++i) {
                code[i] = memory.Read32(getter + i * 4);
            }
        }
        LOG_INFO(Loader,
                 "Pokopia minimap: Pokémon {} component {:#x} (position at +{:#x}), vtable "
                 "main+{:#x}, getter main+{:#x}: {:08x} {:08x} {:08x} {:08x}",
                 id, component, ctx.r[0] - component, vtable - g_main_base, getter - g_main_base,
                 code[0], code[1], code[2], code[3]);
    }
    g_pokemon[id] = TrackedPokemon{ctx.r[0], species, g_pass};
}

template <std::size_t... I>
void AddIconHooks(HookInstaller& hooks, std::index_sequence<I...>) {
    (hooks.Add(IconSites[I].site, &OnIconPlaced<I>, "minimap icons"), ...);
}

void ClearIcons() {
    std::scoped_lock lock{g_icon_mutex};
    g_icons.clear();
    g_pokemon.clear();
}

struct MapMarks {
    struct Mark {
        Vec where; // world x, z
        Place place;
        Color color;
    };
    std::vector<Mark> places;
    std::vector<Vec> pokemon;
};

u64 ReadGlobal(Core::Memory::Memory& memory, u64 got) {
    if (!Valid(memory, g_main_base + got)) {
        return 0;
    }
    const u64 global = memory.Read64(g_main_base + got);
    return Valid(memory, global) ? memory.Read64(global) : 0;
}

/// Where a Pokémon is, straight from the game's tables (no menu needed), if `id` is one.
std::optional<Vec> PokemonPosition(Core::Memory::Memory& memory, u64 list, u64 world, u16 id) {
    if ((id >> 12) != 2 || (id & 0xFFF) >= 64) {
        return std::nullopt;
    }
    const u64 slot_address = list + 0x50 + static_cast<u64>(id & 0xFFF) * 8;
    if (!Valid(memory, slot_address)) {
        return std::nullopt;
    }
    const u64 object = memory.Read64(slot_address);
    if (!Valid(memory, object + TransformSlot, 4) ||
        memory.Read64(object) != g_main_base + PokemonVtable) {
        return std::nullopt;
    }
    const u32 slot = memory.Read32(object + TransformSlot);
    const u64 position = world + TransformTable + static_cast<u64>(slot) * 0x30 + 0x10;
    if (slot >= TransformSlots || !Valid(memory, position, 12)) {
        return std::nullopt;
    }
    return Vec{std::bit_cast<float>(memory.Read32(position)),
               std::bit_cast<float>(memory.Read32(position + 8))};
}

/// The ids in the area's Pokémon list, or nullopt if it cannot be read.
std::optional<std::vector<u16>> PokemonInArea(Core::Memory::Memory& memory) {
    const u64 list = ReadGlobal(memory, PokemonListGlobal);
    if (!Valid(memory, list + 0x2b8, 0x10)) {
        return std::nullopt;
    }
    const u64 ids = memory.Read64(list + 0x2b8);
    const s32 count = static_cast<s32>(memory.Read32(list + 0x2c4));
    if (count <= 0 || count > 4096 || !Valid(memory, ids, static_cast<u64>(count) * 2)) {
        return std::nullopt;
    }
    std::vector<u16> out(static_cast<std::size_t>(count));
    for (s32 i = 0; i < count; ++i) {
        out[i] = memory.Read16(ids + static_cast<u64>(i) * 2);
    }
    return out;
}

/// What the menu last showed, with the Pokémon where they are now.
MapMarks CollectMarks(Core::System& system) {
    MapMarks marks;
    auto& memory = system.ApplicationMemory();
    const auto in_area = PokemonInArea(memory);
    std::scoped_lock lock{g_icon_mutex};
    // Once a new pass has had time to place everything, drop places it no longer showed.
    if (std::chrono::steady_clock::now() - g_pass_start > std::chrono::milliseconds(500)) {
        std::erase_if(g_icons, [](const Icon& icon) { return icon.pass < g_pass; });
    }
    // Pokémon stay while the game lists them in the area (they are followed live).
    if (in_area) {
        std::erase_if(g_pokemon, [&](const auto& entry) {
            return std::ranges::find(*in_area, entry.first) == in_area->end();
        });
    }
    for (const Icon& icon : g_icons) {
        const Color color = icon.id != 0xFFFF ? FacilityColors[icon.id % FacilityColors.size()]
                                              : IconColors[icon.kind];
        marks.places.push_back({Vec{icon.x, icon.z}, PlaceOf(icon.id), color});
    }
    const auto plausible = [](Vec p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) < CellOrigin &&
               std::abs(p.y) < CellOrigin;
    };
    // Every Pokémon in the area, live, from the game's own tables.
    std::vector<u16> found;
    if (in_area) {
        const u64 list = ReadGlobal(memory, PokemonListGlobal);
        const u64 world = ReadGlobal(memory, WorldGlobal);
        for (const u16 id : *in_area) {
            if (const auto p = PokemonPosition(memory, list, world, id); p && plausible(*p)) {
                marks.pokemon.push_back(*p);
                found.push_back(id);
            }
        }
    }
    // Others the menu showed (a kind of Pokémon those tables do not cover).
    for (const auto& [id, tracked] : g_pokemon) {
        if (std::ranges::find(found, id) != found.end() || !Valid(memory, tracked.position, 12)) {
            continue;
        }
        const Vec p{std::bit_cast<float>(memory.Read32(tracked.position)),
                    std::bit_cast<float>(memory.Read32(tracked.position + 8))};
        if (plausible(p)) {
            marks.pokemon.push_back(p);
        }
    }
    return marks;
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
    // Controller: L3+R3 in play pings; on the pause menu's map the right stick moves a cursor.
    bool prev_r3 = false, prev_chord = false;
    bool map_cursor = false;
    float cursor_u = 0.5f, cursor_v = 0.5f;
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

/// The ping on a map: a pin whose point is the spot.
void PinMarker(Canvas& c, Vec p) {
    const Vec head{p.x, p.y - 11.f};
    c.Disc(Vec{p.x, p.y + 0.5f}, 2.5f, Color{0.f, 0.f, 0.f, 0.3f});
    c.Triangle(p + Vec{0.f, 2.f}, head + Vec{-7.5f, 1.f}, head + Vec{7.5f, 1.f}, White);
    c.Disc(head, 8.f, White);
    c.Triangle(p, head + Vec{-5.5f, 1.f}, head + Vec{5.5f, 1.f}, Ping);
    c.Disc(head, 6.f, Ping);
    c.Disc(head, 2.4f, White);
}

void DrawPing(Canvas& c, Vec at, Vec centre, float limit_x0, float limit_y0, float limit_x1,
              float limit_y1) {
    if (at.x >= limit_x0 && at.x <= limit_x1 && at.y >= limit_y0 && at.y <= limit_y1) {
        PinMarker(c, at);
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

void Draw(State& s, const CameraInfo& cam, Vec forward, const PanelView& view,
          const MapMarks& marks) {
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
        const auto inside = [x1, y1](Vec p) {
            return p.x > PanelX0 + 2.f && p.x < x1 - 2.f && p.y > PanelY0 + 2.f && p.y < y1 - 2.f;
        };
        for (const auto& mark : marks.places) {
            const Vec p = view.ToPanel(mark.where.x, mark.where.y);
            if (inside(p) && Length(p - at) > 4.f) {
                FacilityMarker(c, p, mark.place, mark.color);
            }
        }
        for (const Vec& where : marks.pokemon) {
            const Vec p = view.ToPanel(where.x, where.y);
            if (inside(p)) {
                PokemonMarker(c, p);
            }
        }
        if (s.ping) {
            DrawPing(c, view.ToPanel(s.ping_x, s.ping_z), at, PanelX0 + 6.f, PanelY0 + 6.f,
                     x1 - 6.f, y1 - 6.f);
        }
        PlayerMarker(c, at, forward);
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
                PinMarker(c, centre + rel);
            }
        }
        PlayerMarker(c, centre, forward);
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
        Pill(c, "HERE", ArrowX, ArrowY + 26.f);
        return;
    }
    const Vec right{-forward.y, forward.x};
    const float ahead = to_ping.x * forward.x + to_ping.y * forward.y;
    const float across = to_ping.x * right.x + to_ping.y * right.y;
    Vec screen{across, -ahead};
    screen = screen * (1.f / std::max(Length(screen), 1e-4f));
    GuideArrow(c, arrow, screen, Ping);
    Pill(c, Distance(dist), ArrowX, ArrowY + 28.f);
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

void MenuMapPinsAndCursor(Canvas& c, const State& s, Vec at, Vec ping, bool ping_on_map) {
    if (ping_on_map) {
        PinMarker(c, ping);
    }
    // A crosshair: a ring with four ticks, dark under white.
    for (const auto& [width, color] : {std::pair{4.f, Dark}, std::pair{2.f, White}}) {
        c.Ring(at, 9.f, width, color);
        const float h = width * 0.5f;
        c.Rect(at.x - 17.f, at.y - h, at.x - 11.f, at.y + h, color);
        c.Rect(at.x + 11.f, at.y - h, at.x + 17.f, at.y + h, color);
        c.Rect(at.x - h, at.y - 17.f, at.x + h, at.y - 11.f, color);
        c.Rect(at.x - h, at.y + 11.f, at.x + h, at.y + 17.f, color);
    }
    c.Disc(at, 2.f, Ping);
    (void)s;
}

struct Pad {
    float rx = 0.f, ry = 0.f;
    bool l3 = false, r3 = false;
};

Pad ReadPad(Core::HID::HIDCore& hid) {
    const auto* controller = hid.GetEmulatedController(hid.GetFirstNpadId());
    if (controller == nullptr) {
        return {};
    }
    const auto sticks = controller->GetSticks();
    const auto buttons = controller->GetNpadButtons();
    return {static_cast<float>(sticks.right.x) / 32767.f,
            static_cast<float>(sticks.right.y) / 32767.f, buttons.stick_l != 0,
            buttons.stick_r != 0};
}

bool MenuMapOnScreen() {
    std::scoped_lock lock{g_menu_map_mutex};
    return std::chrono::steady_clock::now() - g_menu_map.seen <= MenuMapGone;
}

/// Where a world position is on the pause menu's map, 0..1 across it (PingFromMenuMap reversed).
bool MenuMapUv(float x, float z, float* u, float* v) {
    MapInfo map;
    {
        std::scoped_lock lock{g_map_mutex};
        map = g_map;
    }
    if (!map.drawn || map.extent <= 0) {
        return false;
    }
    const float span =
        static_cast<float>(map.extent) * MenuMargin * static_cast<float>(map.cell_px);
    const float tx = ((x + CellOrigin) / CellUnits - map.first_x) * map.cell_px;
    const float ty = ((z + CellOrigin) / CellUnits - map.first_y) * map.cell_px;
    *u = (tx - TextureSize * 0.5f) / span + 0.5f;
    *v = (ty - TextureSize * 0.5f) / span + 0.5f;
    return true;
}

/// On the pause menu's map: a cursor the right stick moves, R3 puts the ping under it (or lifts
/// the ping when it is on it). False when the map is not on screen.
bool MenuMapCursor(State& s, const Pad& pad, bool r3_press) {
    MenuMapRect r;
    {
        std::scoped_lock lock{g_menu_map_mutex};
        r = g_menu_map;
    }
    if (std::chrono::steady_clock::now() - r.seen > MenuMapGone || r.x1 - r.x0 < 8.f ||
        r.y1 - r.y0 < 8.f) {
        s.map_cursor = false;
        return false;
    }
    float pu = 0.f, pv = 0.f;
    const bool ping_on_map = s.ping && MenuMapUv(s.ping_x, s.ping_z, &pu, &pv) && pu >= 0.f &&
                             pu <= 1.f && pv >= 0.f && pv <= 1.f;
    if (!s.map_cursor) {
        s.map_cursor = true;
        s.cursor_u = ping_on_map ? pu : 0.5f;
        s.cursor_v = ping_on_map ? pv : 0.5f;
        s.last_signature = ~u64{0};
    }
    const auto curve = [](float value) {
        constexpr float Dead = 0.2f;
        const float a = std::fabs(value);
        if (a < Dead) {
            return 0.f;
        }
        const float t = std::min((a - Dead) / (1.f - Dead), 1.f);
        return std::copysign(t * t, value);
    };
    constexpr float Speed = 0.7f / 60.f; // map widths a tick at full tilt
    s.cursor_u = std::clamp(s.cursor_u + curve(pad.rx) * Speed, 0.f, 1.f);
    s.cursor_v = std::clamp(s.cursor_v - curve(pad.ry) * Speed, 0.f, 1.f);
    if (r3_press) {
        if (ping_on_map && std::hypot(pu - s.cursor_u, pv - s.cursor_v) < 0.03f) {
            s.ping = false;
            LOG_INFO(Loader, "Pokopia navigation: ping lifted from the menu's map");
        } else {
            PingFromMenuMap(s.cursor_u, s.cursor_v);
        }
        s.last_signature = ~u64{0};
    }

    const float k = static_cast<float>(W) / 1920.f;
    const float x0 = r.x0 * k, y0 = r.y0 * k, x1 = r.x1 * k, y1 = r.y1 * k;
    const Vec at{x0 + s.cursor_u * (x1 - x0), y0 + s.cursor_v * (y1 - y0)};
    const auto q = [](float value) { return static_cast<u64>(static_cast<s64>(value * 4.f)); };
    const u64 signature = q(at.x) * 1000003ULL ^ q(at.y) * 998244353ULL ^ q(x0) * 7919ULL ^
                          q(y0) * 131ULL ^ (s.ping ? q(pu * 512.f) * 31ULL ^ q(pv * 512.f) : 7ULL) ^
                          0x4D41500000000000ULL;
    if (signature == s.last_signature && s.shown) {
        return true;
    }
    s.last_signature = signature;
    Canvas& c = s.canvas;
    c.Clear();
    MenuMapPinsAndCursor(c, s, at, ping_on_map ? Vec{x0 + pu * (x1 - x0), y0 + pv * (y1 - y0)}
                                               : Vec{-100.f, -100.f},
                         ping_on_map);
    Pill(c, "R3 PING", (x0 + x1) * 0.5f, std::min(y1 + 6.f, static_cast<float>(H) - 30.f));
    Common::GetHostOverlay().Publish(c.px);
    s.shown = true;
    return true;
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
    // In play: the camera moves each frame and the pause menu's map is not up. Not the menu
    // mouse's in-game menu detection: a Handy Bag kept on screen ("Always Display Handy Bag")
    // counts as a menu there, and hid the minimap for the whole of play.
    constexpr s64 PlayWindowNs = 150'000'000;
    const bool in_play = have_camera && hid.IsCameraLive(PlayWindowNs) && !MenuMapOnScreen();
    const Pad pad = ReadPad(hid);
    const bool chord = pad.l3 && pad.r3;
    const bool chord_press = chord && !s.prev_chord;
    const bool r3_press = pad.r3 && !s.prev_r3;
    s.prev_chord = chord;
    s.prev_r3 = pad.r3;
    const bool want_ping = hid.TakeNavPing() || (in_play && chord_press);
    if (want_ping && have_camera) {
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

    if (!Settings::values.pokopia_minimap.GetValue()) {
        // Hidden by hand (Settings -> Pikantine, or the Toggle Pokopia Minimap hotkey).
        Hide(s);
        return;
    }
    if (!in_play) {
        if (MenuMapCursor(s, pad, r3_press)) {
            // The pause menu's map is up: the live minimap layer must not cover it.
            overlay.SetGuestTexture(HiddenTexture());
            s.mark_changed = true;
            if (!overlay.Enabled()) {
                overlay.SetEnabled(true);
            }
            return;
        }
        Hide(s);
        return;
    }
    if (s.map_cursor) {
        s.map_cursor = false;
        s.last_signature = ~u64{0};
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
    const MapMarks marks = view.live ? CollectMarks(*g_system) : MapMarks{};

    // Redraw only when something on it moved (the arrival pulse always moves).
    const auto q = [](float v, float step) { return static_cast<u64>(static_cast<s64>(v / step)); };
    u64 signature = q(cam.target[0], 8.f) * 1000003ULL ^ q(cam.target[2], 8.f) * 998244353ULL ^
                    q(std::atan2(forward.y, forward.x), 0.01f) * 7919ULL ^
                    (s.ping ? q(s.ping_x, 1.f) * 31ULL ^ q(s.ping_z, 1.f) * 131ULL ^ 1ULL : 0ULL) ^
                    (view.live ? q(view.crop_x, 1.f) * 65537ULL ^ q(view.crop_y, 1.f) * 257ULL ^ 2ULL
                               : 0ULL);
    for (const Vec& p : marks.pokemon) {
        signature = signature * 1099511628211ULL ^ (q(p.x, 16.f) * 2654435761ULL ^ q(p.y, 16.f));
    }
    signature ^= static_cast<u64>(marks.places.size()) << 48;
    const bool arrived = s.ping && std::hypot(s.ping_x - cam.target[0],
                                              s.ping_z - cam.target[2]) < ArriveRange;
    if (signature == s.last_signature && !arrived && s.shown) {
        return;
    }
    s.last_signature = signature;
    Draw(s, cam, forward, view, marks);
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

void NoteMenuMap(float x0, float y0, float x1, float y1) {
    std::scoped_lock lock{g_menu_map_mutex};
    g_menu_map = {x0, y0, x1, y1, std::chrono::steady_clock::now()};
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
    ClearIcons();
    g_icon_logged = {};
    g_logged_ids.clear();
    g_component_logged = 0;
    AddIconHooks(hooks, std::make_index_sequence<IconSites.size()>{});
    for (const u64 site : PokemonSites) {
        hooks.Add(site, &OnPokemonPlaced, "minimap Pokémon");
    }
    for (const u64 site : PokemonComponentSites) {
        hooks.Add(site, &OnPokemonComponent, "minimap Pokémon component");
    }

    auto& timing = system.CoreTiming();
    if (g_tick_event) {
        timing.UnscheduleEvent(g_tick_event);
    } else {
        g_tick_event = Core::Timing::CreateEvent(
            "Pokopia navigation overlay",
            [](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                // Only while the game runs: once its process has ended (it closed or crashed),
                // its memory is gone and reading it would take the emulator down too.
                if (g_system != nullptr && g_system->ApplicationProcess() != nullptr) {
                    Tick();
                }
                return std::nullopt;
            });
    }
    timing.ScheduleLoopingEvent(TickNs, TickNs, g_tick_event);
    LOG_INFO(Loader, "Pokopia navigation overlay: on (G drops or lifts a ping)");
}

} // namespace Core::Pokopia
