// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/pokemon_cap.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

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

// How many Pokémon the game keeps active (drawn and moving) at once: u32 at +0xc of the block at
// [holder+0] (the holder above: [main+0x36ca678]). 30 by default. Its keeper (main+0x288e80) ranks
// the placed Pokémon nearest first and asks the first 30 to come in (actor+0x2428 |= 1) and the
// rest to go out (|= 2): with more than 30 placed, the farthest were sent out, and the ones out
// when the player left an area stayed out on the next visit (invisible, though placed: Honey then
// finds them "already here"). Raised with the limit, every placed Pokémon stays active.
constexpr u64 ActiveField = 0x0;
constexpr u64 ActiveBudget = 0xc;

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
struct SeenActive {
    u64 block{};
    u32 budget{};
};
SeenActive g_active;
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

// Census (test builds): every placed Pokémon, to see whether the ones that never appear exist.
constexpr u64 WorldGlobal = 0x3483b60;   // GOT -> the world; +0x835af0 the transform table
constexpr u64 TransformTable = 0x835af0;
constexpr u64 TransformSlot = 0x382c;    // u32 in a Pokémon: its row in that table
constexpr u32 TransformSlots = 0x41a;
constexpr u64 ObjKind = 0x39f0, ObjState = 0x27fe, ObjFlags = 0x245e;
u32 g_census_ticks{};
u32 g_census_placed = ~0u;
u32 g_census_stable{};  // censuses since the count last changed
bool g_census_dumped{}; // the objects' layout was logged for this area

// Which 8-byte words of a Pokémon object are non-zero, as hex (one bit a word): a Pokémon that is
// there but not drawn should lack something the drawn ones have (its model, say).
constexpr u64 ObjSize = 0x3d20;
std::string NonZeroWords(Core::Memory::Memory& memory, u64 obj) {
    std::vector<u8> bytes(ObjSize);
    if (!memory.ReadBlock(obj, bytes.data(), bytes.size())) {
        return "unreadable";
    }
    std::string out;
    for (u64 at = 0; at + 32 <= ObjSize; at += 32) {
        u32 nibble = 0;
        for (u32 w = 0; w < 4; ++w) {
            u64 word;
            std::memcpy(&word, bytes.data() + at + w * 8, 8);
            nibble |= word != 0 ? 1u << w : 0;
        }
        out.push_back("0123456789abcdef"[nibble]);
    }
    return out;
}

void Census(Core::Memory::Memory& memory, u64 pool, u32 placed) {
    if (++g_census_ticks < 60 && placed == g_census_placed) {
        return; // every 15 s, or when the count changes
    }
    g_census_ticks = 0;
    g_census_stable = placed == g_census_placed ? g_census_stable + 1 : 0;
    if (placed == 0) {
        g_census_dumped = false;
    }
    const bool dump = !g_census_dumped && placed > 0 && g_census_stable >= 3; // ~45 s in the area
    g_census_dumped |= dump;
    g_census_placed = placed;
    u64 world = 0;
    if (const u64 got = g_main_base + WorldGlobal; Valid(memory, got)) {
        const u64 holder = memory.Read64(got);
        world = Valid(memory, holder) ? memory.Read64(holder) : 0;
    }
    LOG_INFO(Loader, "Pokopia Pokémon census: {} placed", placed);
    for (u32 i = 0; i < PoolSize; ++i) {
        const u64 obj = memory.Read64(pool + PoolSlots + i * 8);
        if (obj == 0 || !Valid(memory, obj + ObjKind, 4)) {
            continue;
        }
        const u32 row = memory.Read32(obj + TransformSlot);
        float x = 0, y = 0, z = 0;
        const u64 pos = world + TransformTable + static_cast<u64>(row) * 0x30 + 0x10;
        if (world != 0 && row < TransformSlots && Valid(memory, pos, 12)) {
            x = std::bit_cast<float>(memory.Read32(pos));
            y = std::bit_cast<float>(memory.Read32(pos + 4));
            z = std::bit_cast<float>(memory.Read32(pos + 8));
        }
        LOG_INFO(Loader,
                 "  slot {:2} resident {:4} kind {:#05x} state {:#06x} flags {:08x} lod {:02x} {:02x} "
                 "{:02x} at {:.0f} {:.0f} {:.0f}",
                 i, row, memory.Read16(obj + ObjKind), memory.Read16(obj + ObjState),
                 memory.Read32(obj + ObjFlags), memory.Read8(obj + 0x2428),
                 memory.Read8(obj + 0x2430), memory.Read8(obj + 0x2431), x, y, z);
        if (dump) {
            LOG_INFO(Loader, "  layout resident {:4}: {}", row, NonZeroWords(memory, obj));
        }
    }
}

// Spawn trace (test builds): every call of the spawner (main+0x28c6c0), what its checks saw and
// whether it made the Pokémon. Honey, area loads and arrivals all go through it.
constexpr u64 SpawnEnterSite = 0x28c6e8;  // mov w8, #0x6608: x0 the list, w1, w2 (kind), x3..
constexpr u64 SpawnLivingSite = 0x28c7d8; // cmp w22, w0: living vs the living limit
constexpr u64 SpawnObjectsSite = 0x28c8e4; // cmp w0, w8: all Pokémon vs the hard limit
constexpr u64 SpawnResultSite = 0x28c92c; // mov x27, x0: the new Pokémon, or null
std::atomic<u32> g_spawn_kind{0};

void OnSpawnEnter(Core::System& system, Kernel::Svc::ThreadContext& ctx) {
    auto& memory = system.ApplicationMemory();
    const u64 list = ctx.r[0];
    const u32 free = Valid(memory, list + 0x6608, 8) ? memory.Read32(list + 0x6608) : ~0u;
    const u32 top = Valid(memory, list + 0x6608, 8) ? memory.Read8(list + 0x660c) : ~0u;
    g_spawn_kind = static_cast<u32>(ctx.r[2] & 0xffff);
    LOG_INFO(Loader, "Pokopia spawn: kind {:#x} (w1 {:#x}), {} slots free, next slot {}",
             ctx.r[2] & 0xffff, ctx.r[1] & 0xffff, free, top);
}
void OnSpawnLiving(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia spawn: kind {:#x}: {} living, limit {}", g_spawn_kind.load(),
             static_cast<s32>(ctx.r[22]), static_cast<s32>(ctx.r[0]));
}
void OnSpawnObjects(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia spawn: kind {:#x}: {} Pokémon in all, hard limit {}",
             g_spawn_kind.load(), static_cast<s32>(ctx.r[0]), static_cast<s32>(ctx.r[8]));
}
void OnSpawnResult(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia spawn: kind {:#x}: {}", g_spawn_kind.load(),
             ctx.r[0] != 0 ? fmt::format("made at {:#x}", ctx.r[0]) : std::string{"NOT made"});
}

// Summon trace (test builds): main+0x1c9dfc(w1 resident) brings one Pokémon in (area fills,
// arrivals, probably Honey); its early exits decide before the spawner is ever called.
constexpr u64 SummonEnterSite = 0x1c9e48;  // mov w0, w1: w1 the Pokémon, w2, w3
constexpr u64 SummonNewSite = 0x1c9e80;    // not in the world yet (the "already there" exit passed)
constexpr u64 SummonStageSite = 0x1c9e9c;  // cmp w8, #0xe1: w0 its area, w19 the area here
constexpr u64 SummonResultSite = 0x1c9f84; // mov x21, x0: what the spawner made
constexpr u64 ArrivalSite = 0x1c9bb8;      // cmp w24, w23 in main+0x1c9ae0: counted vs the limit
std::atomic<u32> g_summon_id{0};

void OnSummonEnter(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    g_summon_id = static_cast<u32>(ctx.r[1] & 0xffff);
    LOG_INFO(Loader, "Pokopia summon: Pokémon {:#x} (w2 {}, w3 {})", ctx.r[1] & 0xffff,
             ctx.r[2] & 0xff, ctx.r[3] & 0xff);
}
void OnSummonNew(Core::System&, Kernel::Svc::ThreadContext&) {
    LOG_INFO(Loader, "Pokopia summon: Pokémon {:#x}: not in the world yet", g_summon_id.load());
}
void OnSummonStage(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia summon: Pokémon {:#x}: its area {:#x}, this area {:#x}",
             g_summon_id.load(), ctx.r[0] & 0xff, ctx.r[19] & 0xff);
}
void OnSummonResult(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia summon: Pokémon {:#x}: {}", g_summon_id.load(),
             ctx.r[0] != 0 ? std::string{"made"} : std::string{"NOT made by the spawner"});
}
void OnArrival(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    LOG_INFO(Loader, "Pokopia arrivals: {} counted, limit {}", static_cast<s32>(ctx.r[24]),
             static_cast<s32>(ctx.r[23]));
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
    {
        u32 placed = 0;
        for (u32 i = 0; i < PoolSize; ++i) {
            placed += memory.Read64(pool + PoolSlots + i * 8) != 0 ? 1 : 0;
        }
        Census(memory, pool, placed);
    }
    ++g_log_ticks;
    if (free != g_logged_free && (g_log_ticks >= 12 || free <= 2)) { // at most every 3 s
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

void WriteActive(Core::Memory::Memory& memory, u32 budget) {
    if (g_active.block == 0 || budget == 0 || !Valid(memory, g_active.block + ActiveBudget, 4)) {
        return;
    }
    if (memory.Read32(g_active.block + ActiveBudget) != budget) {
        memory.Write32(g_active.block + ActiveBudget, budget);
        LOG_INFO(Loader, "Pokopia Pokémon limit: {} active at once", budget);
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
    const u64 active_block = memory.Read64(holder + ActiveField);
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
    if (active_block != g_active.block && Valid(memory, active_block + ActiveBudget, 4)) {
        const u32 budget = memory.Read32(active_block + ActiveBudget);
        if (budget >= 1 && budget <= 64) { // sanity: anything else is not the field we know
            g_active = {active_block, budget};
            LOG_INFO(Loader, "Pokopia Pokémon limit: the game keeps {} active at once", budget);
        }
    }
    if (g_cap == 0) {
        g_fill_cap = ~0u;
        Write(memory, params, g_seen.living); // the game's own limit
        WriteActive(memory, g_active.budget);
        return;
    }
    const u32 target = std::max(g_cap, g_seen.living);
    Write(memory, params, target); // never lowered: that sends Pokémon away
    // Every Pokémon that can be placed stays active (the hard limit), never fewer than the game's.
    WriteActive(memory, std::max(g_active.budget, memory.Read32(params + ObjectLimit)));
    const u32 free = PoolFreeSlots(memory);
    if (!g_staged) {
        g_fill_cap = ~0u;
        return;
    }

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
    g_active = {};
    g_play_ticks = 0;
    g_loaded = false;
    g_fill_owner = 0;
    g_fill_living = 0;
    g_fill_cap = ~0u;
    g_logged_free = ~0u;
    g_log_ticks = 0;
    g_system = &system;
    g_main_base = main_base;
    if (hooks != nullptr) {
        hooks->Add(SpawnEnterSite, &OnSpawnEnter, "Pokémon spawn trace");
        hooks->Add(SpawnLivingSite, &OnSpawnLiving, "Pokémon spawn trace (living)");
        hooks->Add(SpawnObjectsSite, &OnSpawnObjects, "Pokémon spawn trace (all)");
        hooks->Add(SpawnResultSite, &OnSpawnResult, "Pokémon spawn trace (result)");
        hooks->Add(SummonEnterSite, &OnSummonEnter, "Pokémon summon trace");
        hooks->Add(SummonNewSite, &OnSummonNew, "Pokémon summon trace (new)");
        hooks->Add(SummonStageSite, &OnSummonStage, "Pokémon summon trace (area)");
        hooks->Add(SummonResultSite, &OnSummonResult, "Pokémon summon trace (result)");
        hooks->Add(ArrivalSite, &OnArrival, "Pokémon arrivals trace");
    }
    g_cap = static_cast<u32>(std::clamp<long>(requested, 0, MaxCap));
    // Staged (the game's limit while loading, then up in play) needs the fill hook and the
    // camera hook that tells play apart; without them (NCE) the limit is simply raised.
    // Staged filling is opt-in (EDEN_POKOPIA_POKEMON_STAGED=1): a test showed the game places
    // Pokémon only while an area loads, so the in-play steps added none (25 placed per area).
    const char* staged = std::getenv("EDEN_POKOPIA_POKEMON_STAGED");
    g_staged = hooks != nullptr && system.HIDCore().IsMouseCameraHooked() &&
               staged != nullptr && staged[0] == '1' &&
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
