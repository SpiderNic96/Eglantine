// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/pokemon_cap.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/memory.h"

namespace Core::Pokopia {

namespace {

constexpr u64 ParamsGlobal = 0x36ca678; // -> object whose +0x20 holds the game's parameters
constexpr u64 ParamsField = 0x20;
constexpr u64 LivingLimit = 0x4bc;  // Pokémon that may live in the world (20)
constexpr u64 ObjectLimit = 0x4c0;  // all Pokémon objects, the spawner's hard stop
constexpr u32 DefaultCap = 30;
constexpr u32 MaxCap = 50;          // the object pool has 64 slots; leave room for visitors
constexpr u32 MaxObjects = 56;
constexpr std::chrono::nanoseconds CheckNs{std::chrono::milliseconds{500}};

Core::System* g_system{};
u64 g_main_base{};
u32 g_cap{};
std::shared_ptr<Core::Timing::EventType> g_event;

struct Seen {
    u64 params{};
    u32 living{};
    u32 objects{};
};
Seen g_seen;

bool Valid(Core::Memory::Memory& memory, u64 address, u64 size = 8) {
    return address != 0 && memory.IsValidVirtualAddressRange(address, size);
}

void Apply() {
    auto& memory = g_system->ApplicationMemory();
    const u64 global = g_main_base + ParamsGlobal;
    if (!Valid(memory, global)) {
        return;
    }
    const u64 holder = memory.Read64(global);
    if (!Valid(memory, holder + ParamsField)) {
        return;
    }
    const u64 params = memory.Read64(holder + ParamsField);
    if (!Valid(memory, params + LivingLimit, 8)) {
        return;
    }
    const u32 living = memory.Read32(params + LivingLimit);
    const u32 objects = memory.Read32(params + ObjectLimit);
    if (params != g_seen.params) {
        // New parameters (first load, or reloaded): note the game's own values before changing.
        if (living == 0 || living > 64 || objects > 64) {
            return; // not filled in yet
        }
        g_seen = {params, living, objects};
        LOG_INFO(Loader, "Pokopia Pokémon limit: the game allows {} living ({} objects); now {}",
                 living, objects, g_cap);
    }
    const u32 want_living = std::max(g_cap, g_seen.living);
    const u32 headroom = g_seen.objects > g_seen.living ? g_seen.objects - g_seen.living : 0;
    const u32 want_objects =
        std::clamp(std::max(g_seen.objects, want_living + headroom), want_living, MaxObjects);
    if (living != want_living) {
        memory.Write32(params + LivingLimit, want_living);
    }
    if (objects != want_objects) {
        memory.Write32(params + ObjectLimit, want_objects);
    }
}

} // namespace

void InstallPokemonCap(Core::System& system, u64 main_base) {
    const char* value = std::getenv("EDEN_POKOPIA_POKEMON_CAP");
    const long requested = value != nullptr ? std::strtol(value, nullptr, 10) : DefaultCap;
    auto& timing = system.CoreTiming();
    if (g_event) {
        timing.UnscheduleEvent(g_event);
    }
    g_seen = {};
    if (requested <= 0) {
        LOG_INFO(Loader, "Pokopia Pokémon limit: the game's own (EDEN_POKOPIA_POKEMON_CAP=0)");
        return;
    }
    g_system = &system;
    g_main_base = main_base;
    g_cap = static_cast<u32>(std::clamp<long>(requested, 1, MaxCap));
    if (!g_event) {
        g_event = Core::Timing::CreateEvent(
            "Pokopia Pokémon limit",
            [](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                if (g_system != nullptr) {
                    Apply();
                }
                return std::nullopt;
            });
    }
    timing.ScheduleLoopingEvent(CheckNs, CheckNs, g_event);
    LOG_INFO(Loader, "Pokopia Pokémon limit: up to {} (EDEN_POKOPIA_POKEMON_CAP)", g_cap);
}

} // namespace Core::Pokopia
