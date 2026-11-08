// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/nav_overlay.h"

#include <algorithm>
#include <array>
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
#include "core/pokopia/mouse_camera.h"
#include "hid_core/hid_core.h"

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
std::shared_ptr<Core::Timing::EventType> g_tick_event;

struct State {
    Canvas canvas;
    bool shown = false;
    bool ping = false;
    float ping_x = 0.f, ping_y = 0.f, ping_z = 0.f;
    u64 last_signature = ~u64{0};
    u32 frame = 0;
};

State& St() {
    static State state;
    return state;
}

void Draw(State& s, const CameraInfo& cam, Vec forward) {
    Canvas& c = s.canvas;
    c.Clear();
    const Vec player{cam.target[0], cam.target[2]};
    const Vec centre{RadarX, RadarY};

    // Radar, north up as the game's map: world +x right, +z down.
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
        if (s.shown) {
            s.canvas.Clear();
            overlay.Publish(s.canvas.px);
            s.shown = false;
            s.last_signature = ~u64{0};
        }
        return;
    }
    if (!overlay.Enabled()) {
        overlay.SetEnabled(true);
    }

    Vec forward{cam.target[0] - cam.eye[0], cam.target[2] - cam.eye[2]};
    const float flen = Length(forward);
    forward = flen > 1e-3f ? forward * (1.f / flen) : Vec{std::sin(cam.yaw), std::cos(cam.yaw)};

    // Redraw only when something on it moved (the arrival pulse always moves).
    const auto q = [](float v, float step) { return static_cast<u64>(static_cast<s64>(v / step)); };
    u64 signature = q(cam.target[0], 8.f) * 1000003ULL ^ q(cam.target[2], 8.f) * 998244353ULL ^
                    q(std::atan2(forward.y, forward.x), 0.01f) * 7919ULL ^
                    (s.ping ? q(s.ping_x, 1.f) * 31ULL ^ q(s.ping_z, 1.f) * 131ULL ^ 1ULL : 0ULL);
    const bool arrived = s.ping && std::hypot(s.ping_x - cam.target[0],
                                              s.ping_z - cam.target[2]) < ArriveRange;
    if (signature == s.last_signature && !arrived && s.shown) {
        return;
    }
    s.last_signature = signature;
    Draw(s, cam, forward);
    overlay.Publish(s.canvas.px);
    s.shown = true;
}

} // namespace

void InstallNavOverlay(Core::System& system) {
    g_system = &system;
    St().shown = false;
    St().ping = false;
    St().last_signature = ~u64{0};
    St().canvas.Clear();
    Common::GetHostOverlay().Publish(St().canvas.px);

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
