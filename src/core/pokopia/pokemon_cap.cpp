// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/pokemon_cap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "common/logging.h"
#include "common/settings.h"
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
constexpr u32 MaxCap = 50;          // the object pool has 64 slots; leave room for visitors
constexpr u32 MaxObjects = 56;

// The world's residents list (main+0x3561928): +0x2a0 data, +0x2ac count. Empty while loading.
constexpr u64 ResidentsGlobal = 0x3561928;
constexpr u64 ResidentsData = 0x2a0, ResidentsCount = 0x2ac;

// main+0xba8c0, every frame: when the residents count first becomes non-zero (a load) it brings
// Pokémon in up to the limit (main+0x1caa60 with limit - living), then keeps the count at +0x2c0c
// of its object. At its store (str w22, [x19, #0x2c0c]) the object is in x19; clearing that count
// has the game do the same again on the next frame. Just before, at main+0xba980
// (subs w1, w8, w0), w8 is the limit it fills up to and w0 the Pokémon living.
constexpr u64 FillStoreSite = 0xba98c;
constexpr u64 FillAmountSite = 0xba980;
constexpr u64 FillSeen = 0x2c0c;

// The limit itself (the parameters above) is raised all the time: lowering it, even only while an
// area loads, had the game send the Pokémon above it away for good. What is held back is the fill:
// at a load the game brings Pokémon in only up to its own limit (it would load every one of them
// behind the black screen, for minutes), and in play the fill climbs by Step every StepTicks.
constexpr std::chrono::nanoseconds CheckNs{std::chrono::milliseconds{250}};

// The Pokémon object pool ([[main+0x3483bd0]]): 64 slots, +0x50 a pointer per slot, +0x6608 the
// free slots' stack (u32 count). Every Pokémon the game has placed, in any area visited, holds
// one; once none is free the game cannot place more, and Pokémon vanish until a restart. Steps
// stop while SlotReserve or fewer are free.
constexpr u64 PoolGlobal = 0x3483bd0;
constexpr u64 PoolSlots = 0x50, PoolFree = 0x6608;
constexpr u32 PoolSize = 64;
constexpr u32 SlotReserve = 10;
constexpr u32 SettleTicks = 12; // 3 s in play before the first step
constexpr u32 StepTicks = 12;   // then a step every 3 s
constexpr u32 Step = 2;

Core::System* g_system{};
u64 g_main_base{};
u32 g_cap{};
long g_env_cap = -1; // EDEN_POKOPIA_POKEMON_CAP, or -1: the setting decides (it can change in play)
bool g_staged{};
std::shared_ptr<Core::Timing::EventType> g_event;
std::atomic<u64> g_fill_owner{0};
std::atomic<u32> g_fill_cap{~0u};  // the fill goes no higher than this (staged)
std::atomic<u32> g_fill_living{0}; // living Pokémon at the last fill

struct Seen {
    u64 params{};
    u32 living{};
    u32 objects{};
};
Seen g_seen;
u32 g_play_ticks{}; // ticks in play since the residents came back
bool g_loaded{};
u32 g_logged_free = ~0u;
u32 g_log_ticks{};

bool Valid(Core::Memory::Memory& memory, u64 address, u64 size = 8) {
    return address != 0 && memory.IsValidVirtualAddressRange(address, size);
}

void OnFillStore(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    g_fill_owner.store(ctx.r[19], std::memory_order_relaxed);
}

void OnFillAmount(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    const u32 living = static_cast<u32>(ctx.r[0]);
    g_fill_living.store(living, std::memory_order_relaxed);
    const u32 cap = g_fill_cap.load(std::memory_order_relaxed);
    if (static_cast<u32>(ctx.r[8]) > cap) {
        ctx.r[8] = cap; // w1 = w8 - w0: bring in no more than up to the cap
    }
}

/// Free slots in the Pokémon pool, or ~0u if it cannot be read; logs how full it is.
u32 PoolFreeSlots(Core::Memory::Memory& memory) {
    const u64 global = g_main_base + PoolGlobal;
    if (!Valid(memory, global)) {
        return ~0u;
    }
    const u64 holder = memory.Read64(global);
    if (!Valid(memory, holder)) {
        return ~0u;
    }
    const u64 pool = memory.Read64(holder);
    if (!Valid(memory, pool + PoolSlots, PoolSize * 8) || !Valid(memory, pool + PoolFree, 4)) {
        return ~0u;
    }
    const u32 free = memory.Read32(pool + PoolFree);
    if (free > PoolSize) {
        return ~0u;
    }
    ++g_log_ticks;
    if (free != g_logged_free && g_log_ticks >= 20) { // at most every 5 s
        u32 placed = 0;
        for (u32 i = 0; i < PoolSize; ++i) {
            placed += memory.Read64(pool + PoolSlots + i * 8) != 0 ? 1 : 0;
        }
        LOG_INFO(Loader, "Pokopia Pokémon slots: {} of {} in use ({} free), {} living at the last fill",
                 placed, PoolSize, free, g_fill_living.load(std::memory_order_relaxed));
        g_logged_free = free;
        g_log_ticks = 0;
    }
    return free;
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
    const long wanted = g_env_cap >= 0 ? g_env_cap : Settings::values.pokopia_pokemon_cap.GetValue();
    const u32 cap = static_cast<u32>(std::clamp<long>(wanted, 0, MaxCap));
    if (cap != g_cap) {
        LOG_INFO(Loader, "Pokopia Pokémon limit: set to {}",
                 cap == 0 ? std::string{"the game's own"} : std::to_string(cap));
        g_cap = cap;
    }
    if (params != g_seen.params) {
        // New parameters (first load, or reloaded): note the game's own values before changing.
        const u32 living = memory.Read32(params + LivingLimit);
        const u32 objects = memory.Read32(params + ObjectLimit);
        if (living == 0 || living > 64 || objects > 64) {
            return; // not filled in yet
        }
        g_seen = {params, living, objects};
        g_fill_cap = g_staged ? living : ~0u;
        LOG_INFO(Loader, "Pokopia Pokémon limit: the game allows {} living ({} objects); up to {}",
                 living, objects, g_cap);
    }
    if (g_cap == 0) {
        g_fill_cap = ~0u;
        Write(memory, params, g_seen.living); // the game's own limit
        return;
    }
    const u32 target = std::max(g_cap, g_seen.living);
    Write(memory, params, target); // never lowered: that sends Pokémon away
    if (!g_staged) {
        g_fill_cap = ~0u;
        return;
    }

    const u32 free = PoolFreeSlots(memory);
    if (!ResidentsLoaded(memory)) {
        // Loading (or between areas): the game fills only up to its own limit.
        if (g_loaded) {
            LOG_INFO(Loader, "Pokopia Pokémon limit: loading, bringing in up to {} until in play",
                     g_seen.living);
        }
        g_loaded = false;
        g_fill_cap = g_seen.living;
        g_play_ticks = 0;
        return;
    }
    g_loaded = true;
    const u32 fill = g_fill_cap.load(std::memory_order_relaxed);
    if (fill >= target || !g_system->HIDCore().IsCameraLive()) {
        return; // done, or not in play (menus, pause, cutscenes): wait
    }
    ++g_play_ticks;
    if (g_play_ticks < SettleTicks || (g_play_ticks - SettleTicks) % StepTicks != 0) {
        return;
    }
    if (free != ~0u && free <= SlotReserve) {
        LOG_INFO(Loader, "Pokopia Pokémon limit: holding at {}, only {} of {} slots free", fill,
                 free, PoolSize);
        return;
    }
    // From the Pokémon already living (a save can hold more than the game's own limit).
    const u32 living = g_fill_living.load(std::memory_order_relaxed);
    const u32 next = std::min(target, std::max(fill, living) + Step);
    g_fill_cap = next;
    const u64 owner = g_fill_owner.load(std::memory_order_relaxed);
    const bool refill = Valid(memory, owner + FillSeen, 4) && memory.Read32(owner + FillSeen) != 0;
    if (refill) {
        memory.Write32(owner + FillSeen, 0); // the game brings Pokémon in up to the new cap
    }
    LOG_INFO(Loader, "Pokopia Pokémon limit: {}, bringing in up to {}{}", target, next,
             refill ? "" : " (they move in over time)");
}

} // namespace

void InstallPokemonCap(Core::System& system, HookInstaller* hooks, u64 main_base) {
    const char* value = std::getenv("EDEN_POKOPIA_POKEMON_CAP");
    g_env_cap = value != nullptr ? std::max(0L, std::strtol(value, nullptr, 10)) : -1;
    const long requested =
        g_env_cap >= 0 ? g_env_cap : Settings::values.pokopia_pokemon_cap.GetValue();
    auto& timing = system.CoreTiming();
    if (g_event) {
        timing.UnscheduleEvent(g_event);
    }
    g_seen = {};
    g_play_ticks = 0;
    g_loaded = false;
    g_fill_owner = 0;
    g_fill_living = 0;
    g_fill_cap = ~0u;
    g_logged_free = ~0u;
    g_log_ticks = 0;
    g_system = &system;
    g_main_base = main_base;
    g_cap = static_cast<u32>(std::clamp<long>(requested, 0, MaxCap));
    // Staged (the game's limit while loading, then up in play) needs the fill hook and the
    // camera hook that tells play apart; without them (NCE) the limit is simply raised.
    const char* staged = std::getenv("EDEN_POKOPIA_POKEMON_STAGED");
    g_staged = hooks != nullptr && system.HIDCore().IsMouseCameraHooked() &&
               !(staged != nullptr && staged[0] == '0') &&
               hooks->AddInline(FillStoreSite, &OnFillStore, "Pokémon limit (fill)") &&
               hooks->Add(FillAmountSite, &OnFillAmount, "Pokémon limit (fill amount)");
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
    LOG_INFO(Loader, "Pokopia Pokémon limit: {} ({}){}",
             g_cap == 0 ? "the game's own" : "up to " + std::to_string(g_cap),
             g_env_cap >= 0 ? "EDEN_POKOPIA_POKEMON_CAP" : "Settings -> Pikantine",
             g_staged ? ", raised in play, a few at a time" : "");
}

} // namespace Core::Pokopia
