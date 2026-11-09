// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/pokemon_cap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/pokopia/hooks.h"
#include "hid_core/hid_core.h"

namespace Core::Pokopia {

namespace {

constexpr u64 ParamsGlobal = 0x36ca678; // -> object whose +0x20 holds the game's parameters
constexpr u64 ParamsField = 0x20;
constexpr u64 LivingLimit = 0x4bc;  // Pokémon that may live in the world (25)
constexpr u64 ObjectLimit = 0x4c0;  // all Pokémon objects, the spawner's hard stop (30)
constexpr u32 DefaultCap = 30;
constexpr u32 MaxCap = 50;          // the object pool has 64 slots; leave room for visitors
constexpr u32 MaxObjects = 56;

// The world's residents list (main+0x3561928): +0x2a0 data, +0x2ac count. Empty while loading.
constexpr u64 ResidentsGlobal = 0x3561928;
constexpr u64 ResidentsData = 0x2a0, ResidentsCount = 0x2ac;

// main+0xba8c0, every frame: when the residents count first becomes non-zero (a load) it brings
// Pokémon in up to the limit (main+0x1caa60, limit - living), then keeps the count at +0x2c0c of
// its object. At its store (str w22, [x19, #0x2c0c]) the object is in x19. Clearing that count
// has the game do the same again on the next frame, with the limit as it is then.
constexpr u64 FillStoreSite = 0xba98c;
constexpr u64 FillSeen = 0x2c0c;

// The game loads every Pokémon it brings in at once while the screen is black, so a big raise
// made the loading screen take minutes. The game keeps its own limit while it loads; in play the
// limit climbs by Step every StepTicks, and each step brings that many in.
constexpr std::chrono::nanoseconds CheckNs{std::chrono::milliseconds{250}};
constexpr u32 SettleTicks = 12; // 3 s in play before the first step
constexpr u32 StepTicks = 12;   // then a step every 3 s
constexpr u32 Step = 2;

Core::System* g_system{};
u64 g_main_base{};
u32 g_cap{};
bool g_staged{};
std::shared_ptr<Core::Timing::EventType> g_event;
std::atomic<u64> g_fill_owner{0};

struct Seen {
    u64 params{};
    u32 living{};
    u32 objects{};
};
Seen g_seen;
u32 g_level{};      // the limit as it stands now
u32 g_play_ticks{}; // ticks in play since the residents came back
bool g_loaded{};
u32 g_last_count{}; // Pokémon in the world when last in play: a load keeps room for them

bool Valid(Core::Memory::Memory& memory, u64 address, u64 size = 8) {
    return address != 0 && memory.IsValidVirtualAddressRange(address, size);
}

void OnFillStore(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    g_fill_owner.store(ctx.r[19], std::memory_order_relaxed);
}

/// Every Pokémon in the world (main+0x3561928: +0x2b8 ids, +0x2c4 count), or 0.
u32 PokemonCount(Core::Memory::Memory& memory) {
    const u64 global = g_main_base + ResidentsGlobal;
    if (!Valid(memory, global)) {
        return 0;
    }
    const u64 list = memory.Read64(global);
    if (!Valid(memory, list + 0x2b8, 16) || memory.Read64(list + 0x2b8) == 0) {
        return 0;
    }
    const s32 count = static_cast<s32>(memory.Read32(list + 0x2c4));
    return count > 0 && count <= 64 ? static_cast<u32>(count) : 0;
}

bool ResidentsLoaded(Core::Memory::Memory& memory) {
    const u64 global = g_main_base + ResidentsGlobal;
    if (!Valid(memory, global)) {
        return false;
    }
    const u64 list = memory.Read64(global);
    if (!Valid(memory, list + ResidentsData, 16)) {
        return false;
    }
    return memory.Read64(list + ResidentsData) != 0 &&
           static_cast<s32>(memory.Read32(list + ResidentsCount)) >= 1;
}

void Write(Core::Memory::Memory& memory, u64 params, u32 living) {
    const u32 headroom = g_seen.objects > g_seen.living ? g_seen.objects - g_seen.living : 0;
    const u32 objects =
        living == g_seen.living
            ? g_seen.objects
            : std::clamp(std::max(g_seen.objects, living + headroom), living, MaxObjects);
    if (memory.Read32(params + LivingLimit) != living) {
        memory.Write32(params + LivingLimit, living);
    }
    if (memory.Read32(params + ObjectLimit) != objects) {
        memory.Write32(params + ObjectLimit, objects);
    }
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
    if (params != g_seen.params) {
        // New parameters (first load, or reloaded): note the game's own values before changing.
        const u32 living = memory.Read32(params + LivingLimit);
        const u32 objects = memory.Read32(params + ObjectLimit);
        if (living == 0 || living > 64 || objects > 64) {
            return; // not filled in yet
        }
        g_seen = {params, living, objects};
        g_level = living;
        LOG_INFO(Loader, "Pokopia Pokémon limit: the game allows {} living ({} objects); up to {}",
                 living, objects, g_cap);
    }
    const u32 target = std::max(g_cap, g_seen.living);
    if (!g_staged) {
        g_level = target;
        Write(memory, params, g_level);
        return;
    }

    const bool loaded = ResidentsLoaded(memory);
    if (!loaded) {
        // Loading (or between worlds): the game's own limit, so it loads as fast as it does alone,
        // or room for the Pokémon already living (none is sent away for the lower limit).
        g_last_count = std::max(g_last_count, PokemonCount(memory));
        const u32 hold = std::clamp(g_last_count, g_seen.living, target);
        if (g_loaded) {
            LOG_INFO(Loader, "Pokopia Pokémon limit: loading, {} until in play", hold);
        }
        g_loaded = false;
        g_level = hold;
        g_play_ticks = 0;
        Write(memory, params, g_level);
        return;
    }
    g_loaded = true;
    Write(memory, params, g_level);
    if (g_system->HIDCore().IsCameraLive()) {
        g_last_count = std::max(g_last_count, PokemonCount(memory));
    }
    if (g_level >= target || !g_system->HIDCore().IsCameraLive()) {
        return; // done, or not in play (menus, pause, cutscenes): wait
    }
    ++g_play_ticks;
    if (g_play_ticks < SettleTicks || (g_play_ticks - SettleTicks) % StepTicks != 0) {
        return;
    }
    g_level = std::min(target, g_level + Step);
    Write(memory, params, g_level);
    const u64 owner = g_fill_owner.load(std::memory_order_relaxed);
    const bool refill = Valid(memory, owner + FillSeen, 4) && memory.Read32(owner + FillSeen) != 0;
    if (refill) {
        memory.Write32(owner + FillSeen, 0); // the game brings Pokémon in up to the new limit
    }
    LOG_INFO(Loader, "Pokopia Pokémon limit: now {}{}", g_level,
             refill ? ", bringing more in" : " (they move in over time)");
}

} // namespace

void InstallPokemonCap(Core::System& system, HookInstaller* hooks, u64 main_base) {
    const char* value = std::getenv("EDEN_POKOPIA_POKEMON_CAP");
    const long requested = value != nullptr ? std::strtol(value, nullptr, 10) : DefaultCap;
    auto& timing = system.CoreTiming();
    if (g_event) {
        timing.UnscheduleEvent(g_event);
    }
    g_seen = {};
    g_level = 0;
    g_play_ticks = 0;
    g_loaded = false;
    g_last_count = 0;
    g_fill_owner = 0;
    if (requested <= 0) {
        LOG_INFO(Loader, "Pokopia Pokémon limit: the game's own (EDEN_POKOPIA_POKEMON_CAP=0)");
        return;
    }
    g_system = &system;
    g_main_base = main_base;
    g_cap = static_cast<u32>(std::clamp<long>(requested, 1, MaxCap));
    // Staged (the game's limit while loading, then up in play) needs the fill hook and the
    // camera hook that tells play apart; without them (NCE) the limit is simply raised.
    const char* staged = std::getenv("EDEN_POKOPIA_POKEMON_STAGED");
    g_staged = hooks != nullptr && system.HIDCore().IsMouseCameraHooked() &&
               !(staged != nullptr && staged[0] == '0') &&
               hooks->AddInline(FillStoreSite, &OnFillStore, "Pokémon limit (fill)");
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
    LOG_INFO(Loader, "Pokopia Pokémon limit: up to {} (EDEN_POKOPIA_POKEMON_CAP){}", g_cap,
             g_staged ? ", raised in play, a few at a time" : "");
}

} // namespace Core::Pokopia
