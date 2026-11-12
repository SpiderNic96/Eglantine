// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/pokopia_glyph_plan.h"
#include "yuzu/pokopia_controls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QKeySequence>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QString>

#include "common/fs/path_util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "input_common/main.h"
#include "video_core/texture_cache/pokopia_glyphs.h"

namespace Pikantine {

namespace {

namespace PG = VideoCommon::PokopiaGlyphs;

constexpr int SS = 4; // supersampling
const QColor Dark(51, 51, 51);
constexpr int RingAlpha = 191;

// What a glyph shows.
struct Look {
    enum Kind { Keep, Key, Mouse, Face, Disc, Cluster, Arrow, Icon } kind = Keep;
    QString label;  // Key / Disc text; Mouse: left, right, middle, move, wheel; Face: circle,
                    // cross, triangle, square; Arrow: up, down, left, right
    QString label2; // Cluster: the second row
};

// The PlayStation symbols' own colours.
QColor FaceColour(const QString& which) {
    if (which == QStringLiteral("circle")) {
        return QColor(229, 82, 91);
    }
    if (which == QStringLiteral("cross")) {
        return QColor(91, 143, 214);
    }
    if (which == QStringLiteral("triangle")) {
        return QColor(61, 187, 149);
    }
    return QColor(215, 127, 182);
}

QString KeyLabel(int code) {
    switch (code) {
    case Qt::Key_Shift:
        return QStringLiteral("SHIFT");
    case Qt::Key_Control:
        return QStringLiteral("CTRL");
    case Qt::Key_Alt:
        return QStringLiteral("ALT");
    case Qt::Key_Meta:
        return QStringLiteral("WIN");
    case Qt::Key_Space:
        return QStringLiteral("SPACE");
    case Qt::Key_Tab:
        return QStringLiteral("TAB");
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("ENTER");
    case Qt::Key_Escape:
        return QStringLiteral("ESC");
    case Qt::Key_Backspace:
        return QStringLiteral("BACK");
    case Qt::Key_CapsLock:
        return QStringLiteral("CAPS");
    case Qt::Key_Up:
        return QString(QChar(0x2191));
    case Qt::Key_Down:
        return QString(QChar(0x2193));
    case Qt::Key_Left:
        return QString(QChar(0x2190));
    case Qt::Key_Right:
        return QString(QChar(0x2192));
    default:
        break;
    }
    QString text = QKeySequence(code).toString(QKeySequence::NativeText).toUpper();
    if (text.size() > 6) {
        text = text.left(6);
    }
    return text;
}

bool MouseCamera() {
    const char* env = std::getenv("EDEN_POKOPIA_MOUSE_CAMERA");
    return env == nullptr || std::strcmp(env, "0") != 0;
}

/// The device a controller binding belongs to, from the input subsystem's list.
std::optional<Common::ParamPackage> DeviceOf(InputCommon::InputSubsystem* input,
                                              const Common::ParamPackage& param) {
    if (input == nullptr) {
        return std::nullopt;
    }
    for (const auto& device : input->GetInputDevices()) {
        if (device.Get("engine", "") == param.Get("engine", "") &&
            device.Get("guid", "") == param.Get("guid", "") &&
            device.Get("port", 0) == param.Get("port", 0)) {
            return device;
        }
    }
    return std::nullopt;
}

bool IsPlayStation(const Common::ParamPackage& device) {
    const std::string name = device.Get("display", "");
    for (const char* s : {"DualSense", "DualShock", "PS5", "PS4", "PlayStation"}) {
        if (name.find(s) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool SameInput(const Common::ParamPackage& a, const Common::ParamPackage& b) {
    for (const char* key : {"button", "axis", "hat", "direction", "invert"}) {
        if (a.Get(key, "") != b.Get(key, "")) {
            return false;
        }
    }
    return a.Has("button") || a.Has("axis") || a.Has("hat");
}

/// A DualSense button by where it sits: the Switch button that is there by default.
Look PlayStationLook(int physical, int own) {
    using namespace Settings::NativeButton;
    switch (physical) {
    case A:
        return {Look::Face, QStringLiteral("circle"), {}};
    case B:
        return {Look::Face, QStringLiteral("cross"), {}};
    case X:
        return {Look::Face, QStringLiteral("triangle"), {}};
    case Y:
        return {Look::Face, QStringLiteral("square"), {}};
    case L:
        return {Look::Key, QStringLiteral("L1"), {}};
    case R:
        return {Look::Key, QStringLiteral("R1"), {}};
    case ZL:
        return {Look::Key, QStringLiteral("L2"), {}};
    case ZR:
        return {Look::Key, QStringLiteral("R2"), {}};
    case Plus:
        return {Look::Icon, QStringLiteral("options"), {}};
    case Minus:
        return {Look::Icon, QStringLiteral("create"), {}};
    case LStick:
        return {Look::Disc, QStringLiteral("L3"), {}};
    case RStick:
        return {Look::Disc, QStringLiteral("R3"), {}};
    case Home:
        return {Look::Disc, QStringLiteral("PS"), {}};
    case DUp:
    case DDown:
    case DLeft:
    case DRight:
        if (physical == own) {
            return {}; // the game's own D-pad glyph is right
        }
        return {Look::Arrow,
                physical == DUp     ? QStringLiteral("up")
                : physical == DDown ? QStringLiteral("down")
                : physical == DLeft ? QStringLiteral("left")
                                    : QStringLiteral("right"),
                {}};
    default:
        return {};
    }
}

Look ButtonLook(InputCommon::InputSubsystem* input, const Settings::PlayerInput& player,
                int button) {
    const Common::ParamPackage param(player.buttons[button]);
    const std::string engine = param.Get("engine", "");
    const bool keyboard_player =
        Common::ParamPackage(player.buttons[Settings::NativeButton::A]).Get("engine", "") ==
        "keyboard";
    if (engine == "keyboard" && param.Get("code", 0) != 0) {
        return {Look::Key, KeyLabel(param.Get("code", 0)), {}};
    }
    if (engine == "mouse") {
        switch (param.Get("button", -1)) {
        case 0:
            return {Look::Mouse, QStringLiteral("left"), {}};
        case 1:
            return {Look::Mouse, QStringLiteral("right"), {}};
        case 2:
            return {Look::Mouse, QStringLiteral("middle"), {}};
        default:
            return {};
        }
    }
    // Pokopia's mouse camera puts ZR on the left button and ZL on the right one when the
    // keyboard leaves them unbound (core/pokopia).
    if (keyboard_player && MouseCamera() && (engine.empty() || engine == "keyboard")) {
        if (button == Settings::NativeButton::ZR) {
            return {Look::Mouse, QStringLiteral("left"), {}};
        }
        if (button == Settings::NativeButton::ZL) {
            return {Look::Mouse, QStringLiteral("right"), {}};
        }
    }
    if (engine == "sdl") {
        const auto device = DeviceOf(input, param);
        if (!device || !IsPlayStation(*device)) {
            return {}; // other pads keep the Switch glyphs
        }
        // Which physical button it is: the Switch button that is on it by default. The default
        // mapping also puts SL/SR on the shoulders, so only the main buttons are asked, the
        // button's own first.
        const auto defaults = input->GetButtonMappingForDevice(*device);
        using namespace Settings::NativeButton;
        static constexpr int order[] = {A,     B,  X,  Y,     L,      R,      ZL,   ZR,    Plus,
                                        Minus, LStick, RStick, DUp, DDown, DLeft, DRight, Home};
        const auto matches = [&](int native) {
            const auto it = defaults.find(static_cast<Values>(native));
            return it != defaults.end() && SameInput(param, it->second);
        };
        if (matches(button)) {
            return PlayStationLook(button, button);
        }
        for (const int native : order) {
            if (matches(native)) {
                return PlayStationLook(native, button);
            }
        }
    }
    return {};
}

Look StickLook(const Settings::PlayerInput& player, int analog) {
    const Common::ParamPackage param(player.analogs[analog]);
    const bool keyboard_player =
        Common::ParamPackage(player.buttons[Settings::NativeButton::A]).Get("engine", "") ==
        "keyboard";
    if (analog == Settings::NativeAnalog::RStick && keyboard_player && MouseCamera()) {
        return {Look::Mouse, QStringLiteral("move"), {}};
    }
    if (param.Get("engine", "") != "analog_from_button") {
        return {};
    }
    QString keys[4];
    const char* dirs[4] = {"up", "left", "down", "right"};
    for (int i = 0; i < 4; ++i) {
        const Common::ParamPackage key(param.Get(dirs[i], ""));
        if (key.Get("engine", "") != "keyboard" || key.Get("code", 0) == 0) {
            return {};
        }
        keys[i] = KeyLabel(key.Get("code", 0));
        if (keys[i].size() != 1) {
            return {};
        }
    }
    return {Look::Cluster, keys[0], keys[1] + keys[2] + keys[3]};
}

// --- Drawing (the game's style: white caps, a 75% dark outline, dark lettering) ----------------

QImage Canvas(int w, int h) {
    QImage img(w * SS, h * SS, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

QFont Bold(qreal pixels) {
    QFont f;
    f.setBold(true);
    f.setWeight(QFont::Black);
    f.setPixelSize(std::max(1, static_cast<int>(pixels)));
    return f;
}

/// Draws text as large as fits in `box`, centred.
void Fit(QPainter& p, const QString& text, const QRectF& box, const QColor& colour) {
    qreal size = box.height();
    QFont f = Bold(size);
    while (size > 4) {
        f = Bold(size);
        const QRectF r = QFontMetricsF(f).tightBoundingRect(text);
        if (r.width() <= box.width() && r.height() <= box.height()) {
            break;
        }
        size -= SS / 2.0;
    }
    p.setFont(f);
    const QRectF r = QFontMetricsF(f).tightBoundingRect(text);
    p.setPen(colour);
    p.drawText(QPointF(box.center().x() - r.width() / 2 - r.left(),
                       box.center().y() - r.height() / 2 - r.top()),
               text);
}

QRectF Cap(QPainter& p, int W, int H) {
    const qreal ring = 5 * SS;
    const qreal radius = std::min(W, H) / 4.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(Dark.red(), Dark.green(), Dark.blue(), RingAlpha));
    p.drawRoundedRect(QRectF(0, 0, W, H), radius, radius);
    p.setBrush(Qt::white);
    const QRectF inner(ring, ring, W - 2 * ring, H - 2 * ring);
    p.drawRoundedRect(inner, std::max(1.0, radius - ring), std::max(1.0, radius - ring));
    return inner;
}

QRectF Disc(QPainter& p, int W, int H) {
    const qreal ring = 5 * SS;
    const qreal size = std::min(W, H);
    const QRectF outer((W - size) / 2, (H - size) / 2, size, size);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(Dark.red(), Dark.green(), Dark.blue(), RingAlpha));
    p.drawEllipse(outer);
    p.setBrush(Qt::white);
    p.drawEllipse(outer.adjusted(ring, ring, -ring, -ring));
    return outer;
}

void Symbol(QPainter& p, const QString& which, const QRectF& box, const QColor& colour) {
    const qreal width = std::max<qreal>(SS, box.width() * 0.17);
    QPen pen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF c = box.center();
    const qreal r = box.width() / 2;
    if (which == QStringLiteral("circle")) {
        p.drawEllipse(c, r, r);
    } else if (which == QStringLiteral("square")) {
        const qreal q = r * 0.84;
        p.drawRect(QRectF(c.x() - q, c.y() - q, 2 * q, 2 * q));
    } else if (which == QStringLiteral("cross")) {
        const qreal q = r * 0.92;
        p.drawLine(QPointF(c.x() - q, c.y() - q), QPointF(c.x() + q, c.y() + q));
        p.drawLine(QPointF(c.x() - q, c.y() + q), QPointF(c.x() + q, c.y() - q));
    } else {
        QPainterPath tri;
        tri.moveTo(c.x(), c.y() - r);
        tri.lineTo(c.x() - r * 0.98, c.y() + r * 0.72);
        tri.lineTo(c.x() + r * 0.98, c.y() + r * 0.72);
        tri.closeSubpath();
        p.drawPath(tri);
    }
}

void Triangle(QPainter& p, const QPointF& c, qreal r, int dir) {
    // dir: 0 up, 1 down, 2 left, 3 right
    QPainterPath t;
    static const qreal pts[4][6] = {{0, -1, -0.9, 0.6, 0.9, 0.6},
                                    {0, 1, -0.9, -0.6, 0.9, -0.6},
                                    {-1, 0, 0.6, -0.9, 0.6, 0.9},
                                    {1, 0, -0.6, -0.9, -0.6, 0.9}};
    t.moveTo(c.x() + pts[dir][0] * r, c.y() + pts[dir][1] * r);
    t.lineTo(c.x() + pts[dir][2] * r, c.y() + pts[dir][3] * r);
    t.lineTo(c.x() + pts[dir][4] * r, c.y() + pts[dir][5] * r);
    t.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(Dark);
    p.drawPath(t);
}

void DrawMouse(QPainter& p, int W, int H, const QString& which) {
    const qreal ring = 5 * SS;
    const qreal mw = std::min(W, H) * (which == QStringLiteral("move") ? 0.62 : 0.8);
    const qreal mh = H * 0.98;
    const QRectF body((W - mw) / 2, (H - mh) / 2, mw, mh);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(Dark.red(), Dark.green(), Dark.blue(), RingAlpha));
    p.drawRoundedRect(body, mw / 2, mw / 2);
    const QRectF inner = body.adjusted(ring, ring, -ring, -ring);
    QPainterPath shape;
    shape.addRoundedRect(inner, mw / 2 - ring, mw / 2 - ring);
    p.setBrush(Qt::white);
    p.drawPath(shape);
    const qreal split = inner.top() + inner.height() * 0.42;
    const qreal cx = inner.center().x();
    if (which == QStringLiteral("left") || which == QStringLiteral("right") ||
        which == QStringLiteral("middle")) {
        p.save();
        p.setClipPath(shape);
        p.setBrush(Dark);
        if (which == QStringLiteral("middle")) {
            const qreal ww = inner.width() * 0.16;
            p.drawRoundedRect(QRectF(cx - ww / 2, inner.top() + inner.height() * 0.1, ww,
                                     inner.height() * 0.24),
                              ww / 2, ww / 2);
        } else {
            p.drawRect(which == QStringLiteral("left")
                           ? QRectF(inner.left(), inner.top(), cx - inner.left(), split - inner.top())
                           : QRectF(cx, inner.top(), inner.right() - cx, split - inner.top()));
        }
        p.restore();
    }
    p.setPen(QPen(Dark, 2 * SS));
    p.drawLine(QPointF(inner.left(), split), QPointF(inner.right(), split));
    p.drawLine(QPointF(cx, inner.top()), QPointF(cx, split));
    if (which == QStringLiteral("move")) {
        const qreal a = 7 * SS;
        const qreal my = inner.center().y() + 4 * SS;
        Triangle(p, QPointF(cx - mw / 2 - a, my), a, 2);
        Triangle(p, QPointF(cx + mw / 2 + a, my), a, 3);
    }
}

/// The glyph for `look`, w x h, or a null image to keep the game's own.
QImage Draw(const Look& look, int w, int h) {
    if (look.kind == Look::Keep) {
        return {};
    }
    QImage img = Canvas(w, h);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const int W = w * SS, H = h * SS;
    switch (look.kind) {
    case Look::Key: {
        const QRectF inner = Cap(p, W, H);
        const bool one = look.label.size() == 1;
        const qreal pad = (one ? 3 : 2) * SS;
        const qreal vpad = one ? 2 * SS : H / 6.0;
        Fit(p, look.label, inner.adjusted(pad, vpad, -pad, -vpad), Dark);
        break;
    }
    case Look::Cluster: {
        const QRectF inner = Cap(p, W, H).adjusted(3 * SS, 3 * SS, -3 * SS, -3 * SS);
        const qreal mid = inner.center().y();
        Fit(p, look.label, QRectF(inner.left(), inner.top(), inner.width(), mid - SS - inner.top()),
            Dark);
        Fit(p, look.label2, QRectF(inner.left(), mid + SS, inner.width(), inner.bottom() - mid - SS),
            Dark);
        break;
    }
    case Look::Mouse:
        DrawMouse(p, W, H, look.label);
        break;
    case Look::Face: {
        const QRectF outer = Disc(p, W, H);
        const qreal inset = outer.width() * 0.27;
        Symbol(p, look.label, outer.adjusted(inset, inset, -inset, -inset), FaceColour(look.label));
        break;
    }
    case Look::Disc: {
        const QRectF outer = Disc(p, W, H);
        const qreal pad = 5 * SS + outer.width() / 6;
        Fit(p, look.label, outer.adjusted(pad, pad + 2 * SS, -pad, -pad - 2 * SS), Dark);
        break;
    }
    case Look::Arrow: {
        const QRectF outer = Disc(p, W, H);
        const int dir = look.label == QStringLiteral("up")     ? 0
                        : look.label == QStringLiteral("down") ? 1
                        : look.label == QStringLiteral("left") ? 2
                                                               : 3;
        Triangle(p, outer.center(), outer.width() * 0.22, dir);
        break;
    }
    case Look::Icon: {
        // The DualSense's own marks: three bars for Options, three rays for Create.
        const QRectF inner = Cap(p, W, H);
        const QPointF c = inner.center();
        const qreal unit = std::min(inner.width(), inner.height());
        p.setPen(QPen(Dark, unit * 0.12, Qt::SolidLine, Qt::RoundCap));
        if (look.label == QStringLiteral("options")) {
            const qreal half = unit * 0.26;
            for (int i = -1; i <= 1; ++i) {
                const qreal y = c.y() + i * unit * 0.2;
                p.drawLine(QPointF(c.x() - half, y), QPointF(c.x() + half, y));
            }
        } else {
            p.setPen(QPen(Dark, unit * 0.1, Qt::SolidLine, Qt::RoundCap));
            const QPointF base(c.x(), c.y() + unit * 0.3);
            const qreal len = unit * 0.56;
            for (const qreal degrees : {-48.0, 0.0, 48.0}) {
                const qreal r = degrees * 3.14159265358979 / 180.0;
                const QPointF dir(std::sin(r), -std::cos(r));
                p.drawLine(base + dir * (len * 0.5), base + dir * len);
            }
        }
        break;
    }
    case Look::Keep:
        break;
    }
    p.end();
    return img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGBA8888);
}

PG::Tile ToTile(const QImage& img, int x, int y, std::array<int, 4> clear) {
    PG::Tile t;
    t.x = x;
    t.y = y;
    t.width = img.width();
    t.height = img.height();
    t.clear = clear;
    t.rgba.resize(static_cast<std::size_t>(t.width) * t.height * 4);
    for (int row = 0; row < t.height; ++row) {
        std::memcpy(t.rgba.data() + static_cast<std::size_t>(row) * t.width * 4,
                    img.constScanLine(row), static_cast<std::size_t>(t.width) * 4);
    }
    return t;
}

/// The title's A disc: a white key or disc with the label or symbol cut out to the art's
/// translucent dark, in the art's soft dark glow.
std::optional<PG::Tile> TitleTile(const Look& look) {
    if (look.kind != Look::Key && look.kind != Look::Face && look.kind != Look::Disc) {
        return std::nullopt;
    }
    const auto& d = PG::TitleDisc;
    const int m = 10; // the glow's reach
    const int w = d.x1 - d.x0, h = d.y1 - d.y0;
    const int W = (w + 2 * m) * SS, H = (h + 2 * m) * SS;
    const QRectF box(m * SS, m * SS, w * SS, h * SS);
    const bool round = look.kind != Look::Key;

    QImage shape(W, H, QImage::Format_ARGB32_Premultiplied);
    shape.fill(Qt::transparent);
    {
        QPainter p(&shape);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        if (round) {
            p.drawEllipse(box);
        } else {
            p.drawRoundedRect(box, w * SS / 4.0, w * SS / 4.0);
        }
    }
    // the glow: the shape, blurred, at 22% dark
    QImage glow(W, H, QImage::Format_ARGB32_Premultiplied);
    glow.fill(Qt::transparent);
    {
        QPainter p(&glow);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 8; i >= 1; --i) {
            const qreal grow = i * SS * 0.9;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(13, 11, 9, 9));
            const QRectF r = box.adjusted(-grow, -grow, grow, grow);
            if (round) {
                p.drawEllipse(r);
            } else {
                p.drawRoundedRect(r, w * SS / 4.0 + grow, w * SS / 4.0 + grow);
            }
        }
    }
    QImage mark(W, H, QImage::Format_ARGB32_Premultiplied);
    mark.fill(Qt::transparent);
    {
        QPainter p(&mark);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        if (look.kind == Look::Face) {
            const qreal inset = box.width() * 0.27;
            Symbol(p, look.label, box.adjusted(inset, inset, -inset, -inset), Qt::white);
        } else {
            const qreal pad = 7 * SS;
            Fit(p, look.label, box.adjusted(pad, pad, -pad, -pad), Qt::white);
        }
    }
    QImage out(W, H, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.drawImage(0, 0, glow);
        p.drawImage(0, 0, shape);
    }
    // cut the mark out of the white to the art's translucent dark (12, 12, 12 at 30%)
    for (int y = 0; y < H; ++y) {
        auto* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        const auto* k = reinterpret_cast<const QRgb*>(mark.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            const int a = qAlpha(k[x]);
            if (a == 0) {
                continue;
            }
            const QRgb base = qUnpremultiply(o[x]);
            const auto mix = [a](int from, int to) { return (from * (255 - a) + to * a) / 255; };
            o[x] = qPremultiply(qRgba(mix(qRed(base), 12), mix(qGreen(base), 12),
                                      mix(qBlue(base), 12), mix(qAlpha(base), 77)));
        }
    }
    const QImage small = out.scaled(w + 2 * m, h + 2 * m, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGBA8888);
    return ToTile(small, d.x0 - m, d.y0 - m, {d.x0 - m + 4, d.y0 - m, d.x1 + m - 4, d.y1 + m});
}

int ButtonOf(const char* name) {
    using namespace Settings::NativeButton;
    static const std::pair<const char*, int> names[] = {
        {"A", A},         {"B", B},           {"X", X},         {"Y", Y},
        {"L", L},         {"R", R},           {"ZL", ZL},       {"ZR", ZR},
        {"Plus", Plus},   {"Minus", Minus},   {"DLeft", DLeft}, {"DRight", DRight},
        {"DUp", DUp},     {"DDown", DDown},   {"LStickPress", LStick},
        {"RStickPress", RStick},
    };
    for (const auto& [n, b] : names) {
        if (std::strcmp(n, name) == 0) {
            return b;
        }
    }
    return -1;
}

} // namespace

namespace {

struct Plan {
    std::vector<PG::Tile> atlas;
    std::vector<PG::Tile> title;
};

/// The tiles for every glyph, given what each box should show.
template <typename LookOf>
Plan BuildPlan(LookOf look_of) {
    Plan plan;
    for (const auto& box : PG::AtlasBoxes) {
        const Look look = look_of(box);
        const QImage img = Draw(look, box.x1 - box.x0, box.y1 - box.y0);
        if (img.isNull()) {
            continue;
        }
        plan.atlas.push_back(
            ToTile(img, box.x0, box.y0, {box.x0 - 2, box.y0 - 2, box.x1 + 2, box.y1 + 2}));
        if (std::strcmp(box.name, "A") == 0) {
            if (auto t = TitleTile(look)) {
                plan.title.push_back(std::move(*t));
            }
        }
    }
    return plan;
}

Plan PlanForPlayer(InputCommon::InputSubsystem* input, const Settings::PlayerInput& player) {
    return BuildPlan([&](const PG::Box& box) {
        if (std::strcmp(box.name, "LStick") == 0) {
            return StickLook(player, Settings::NativeAnalog::LStick);
        }
        if (std::strcmp(box.name, "RStick") == 0) {
            return StickLook(player, Settings::NativeAnalog::RStick);
        }
        if (const int button = ButtonOf(box.name); button >= 0) {
            return ButtonLook(input, player, button);
        }
        return Look{};
    });
}

/// A DualSense with its default binds: every button where it sits.
Plan PlanForDualSense() {
    return BuildPlan([](const PG::Box& box) {
        const int button = ButtonOf(box.name);
        return button >= 0 ? PlayStationLook(button, button) : Look{};
    });
}

const QString KbmProfileResource = QStringLiteral(":/pikantine/kbm_profile.ini");

/// The keyboard and mouse profile shipped inside Eglantine, read as Player 1's settings.
Settings::PlayerInput KbmProfilePlayer() {
    Settings::PlayerInput player{};
    QSettings ini(KbmProfileResource, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("Controls"));
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        player.buttons[i] =
            ini.value(QString::fromUtf8(Settings::NativeButton::mapping[i])).toString().toStdString();
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        player.analogs[i] =
            ini.value(QString::fromUtf8(Settings::NativeAnalog::mapping[i])).toString().toStdString();
    }
    player.connected = true;
    return player;
}

QString PokopiaModFolder() {
    return QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400A3A01EE4C000"));
}

bool HasTextures(const QString& mod) {
    const QDir textures(QDir(PokopiaModFolder()).filePath(mod + QStringLiteral("/textures")));
    return textures.exists(QStringLiteral("abafc68a5cb224c9_1024x1024.bin")) &&
           textures.exists(QStringLiteral("eef4c6cb94636c58_1024x1024.bin"));
}

void WriteReadme(const QString& mod, const QString& text) {
    const QString dir = QDir(PokopiaModFolder()).filePath(mod);
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("README.txt")));
    if (!f.exists() && f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(text.toUtf8());
    }
}

/// The two icon mods, made from this copy of the game's own textures the first time they load.
std::vector<PG::Export> IconModExports(InputCommon::InputSubsystem* input) {
    std::vector<PG::Export> out;
    // Which of the two the renderer loads follows Settings -> Pikantine (texture_mods.cpp).
    const auto add = [&](const char* name, Plan plan, const QString& readme) {
        const QString mod = QString::fromUtf8(name);
        if (HasTextures(mod)) {
            return;
        }
        WriteReadme(mod, readme);
        PG::Export e;
        e.folder = std::filesystem::path(
            QDir(PokopiaModFolder()).filePath(mod + QStringLiteral("/textures")).toStdU16String());
        e.atlas = std::move(plan.atlas);
        e.title = std::move(plan.title);
        out.push_back(std::move(e));
    };
    add("Keyboard and mouse glyphs", PlanForPlayer(input, KbmProfilePlayer()),
        QStringLiteral(
            "Pokopia keyboard and mouse glyphs, made by Eglantine (Pikantine) from your own copy "
            "of the game the first time it started.\n\nThe prompts show the \"Pokopia keyboard "
            "and mouse\" profile's keys: A=E B=Shift X=Tab Y=F L=Q R=Space ZL=right click "
            "ZR=left click +=M -=C D-pad=1/2/3/4, sticks WASD and the mouse, stick clicks "
            "Ctrl/V.\nSettings -> Pikantine -> Controls and button icons picks which icon set "
            "is on. Delete this folder and Eglantine makes it again.\nMade from your own game's "
            "textures: do not share.\n"));
    add("PlayStation glyphs", PlanForDualSense(),
        QStringLiteral(
            "Pokopia PlayStation glyphs, made by Eglantine (Pikantine) from your own copy of the "
            "game the first time it started.\n\nA DualSense by position: A=Circle B=Cross "
            "X=Triangle Y=Square L/R=L1/R1 ZL/ZR=L2/R2 +=Options -=Create stick clicks=L3/R3.\n"
            "Settings -> Pikantine -> Controls and button icons picks which icon set is on. "
            "Delete this folder and Eglantine makes it again.\nMade from your own game's "
            "textures: do not share.\n"));
    return out;
}

} // namespace

void InstallPokopiaProfiles() {
    PokopiaControls::InstallProfiles();
}

void SetPokopiaGlyphPlan(InputCommon::InputSubsystem* input, bool pokopia) {
    if (!pokopia) {
        PG::SetPlan({}, {});
        PG::SetExports({});
        return;
    }
    PG::SetExports(IconModExports(input));
    if (Settings::values.pokopia_icons.GetValue() == 0) {
        PG::SetPlan({}, {});
        return;
    }
    const auto& players = Settings::values.players.GetValue();
    // Player 1, or the handheld pad when Player 1 is off.
    const Settings::PlayerInput& player = players[0].connected || !players[8].connected
                                              ? players[0]
                                              : players[8];
    Plan plan = PlanForPlayer(input, player);
    PG::SetPlan(std::move(plan.atlas), std::move(plan.title));
}

} // namespace Pikantine
