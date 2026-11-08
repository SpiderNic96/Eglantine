// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/pokopia/hooks.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core::Pokopia {

namespace {

constexpr u32 SvcHook = 0xD4000001 | (0x7d << 5);

s64 SignExtend(u64 value, int bits) {
    const u64 sign = u64{1} << (bits - 1);
    return static_cast<s64>((value ^ sign) - sign);
}

std::optional<u32> Branch(u64 from, u64 to, bool link = false) {
    const s64 offset = static_cast<s64>(to - from);
    if ((offset & 3) != 0 || offset < -(s64{1} << 27) || offset >= (s64{1} << 27)) {
        return std::nullopt;
    }
    return (link ? 0x94000000U : 0x14000000U) | static_cast<u32>((offset >> 2) & 0x3FFFFFF);
}

// The replaced instruction, rewritten to run at `to` instead of `from`: as it is unless it reads
// the PC. Literal loads are not handled.
std::optional<std::vector<u32>> Relocate(u32 insn, u64 from, u64 to) {
    if ((insn & 0x7C000000) == 0x14000000) { // b, bl
        const u64 target = from + SignExtend(insn & 0x3FFFFFF, 26) * 4;
        const auto branch = Branch(to, target, (insn & 0x80000000) != 0);
        if (!branch) {
            return std::nullopt;
        }
        return std::vector<u32>{*branch};
    }
    const bool cond_branch = (insn & 0xFF000010) == 0x54000000;  // b.cond
    const bool compare_branch = (insn & 0x7E000000) == 0x34000000; // cbz, cbnz
    const bool test_branch = (insn & 0x7E000000) == 0x36000000;    // tbz, tbnz
    if (cond_branch || compare_branch || test_branch) {
        // The same test, jumping 8 bytes ahead to a far branch to the old target; otherwise
        // falling through to a branch over it.
        u64 target;
        u32 near;
        if (test_branch) {
            target = from + SignExtend((insn >> 5) & 0x3FFF, 14) * 4;
            near = (insn & ~(0x3FFFU << 5)) | (2U << 5);
        } else {
            target = from + SignExtend((insn >> 5) & 0x7FFFF, 19) * 4;
            near = (insn & ~(0x7FFFFU << 5)) | (2U << 5);
        }
        const auto far = Branch(to + 8, target);
        if (!far) {
            return std::nullopt;
        }
        return std::vector<u32>{near, 0x14000002U, *far}; // b.cond +8; b +8; b target
    }
    if ((insn & 0x1F000000) == 0x10000000) { // adr, adrp
        const u32 rd = insn & 31;
        const u64 imm = ((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3);
        const u64 value = (insn & 0x80000000) != 0
                              ? (from & ~u64{0xFFF}) + (SignExtend(imm, 21) << 12)
                              : from + SignExtend(imm, 21);
        std::vector<u32> out;
        for (u32 hw = 0; hw < 4; ++hw) {
            const u32 part = static_cast<u32>((value >> (hw * 16)) & 0xFFFF);
            out.push_back((hw == 0 ? 0xD2800000U : 0xF2800000U) | (hw << 21) | (part << 5) | rd);
        }
        return out;
    }
    if ((insn & 0x3B000000) == 0x18000000) { // ldr (literal), prfm (literal)
        return std::nullopt;
    }
    return std::vector<u32>{insn};
}

u32 ReadWord(std::span<const u8> image, std::size_t offset) {
    u32 word;
    std::memcpy(&word, image.data() + offset, sizeof(word));
    return word;
}

void WriteWord(std::span<u8> image, std::size_t offset, u32 word) {
    std::memcpy(image.data() + offset, &word, sizeof(word));
}

// STR (immediate, unsigned offset), any size, general registers: size:111001:00:imm12:Rn:Rt.
bool IsPlainStore(u32 word) {
    return (word & 0x3FC00000U) == 0x39000000U;
}

struct InlineHook {
    Kernel::Svc::GuestHook hook;
    u32 original;
};

std::unordered_map<u64, InlineHook>& InlineHooks() {
    static std::unordered_map<u64, InlineHook> hooks;
    return hooks;
}

u64 Reg(const Kernel::Svc::ThreadContext& ctx, u32 index, bool sp) {
    if (index < 29) {
        return ctx.r[index];
    }
    if (index == 29) {
        return ctx.fp;
    }
    if (index == 30) {
        return ctx.lr;
    }
    return sp ? ctx.sp : 0;
}

void RunInline(Core::System& system, Kernel::Svc::ThreadContext& ctx) {
    const u64 site = ctx.pc - 4;
    const auto it = InlineHooks().find(site);
    if (it == InlineHooks().end()) {
        return;
    }
    it->second.hook(system, ctx);
    const u32 word = it->second.original;
    const u32 size = word >> 30;
    const u64 address = Reg(ctx, (word >> 5) & 31, true) + (u64{(word >> 10) & 0xFFF} << size);
    const u64 value = Reg(ctx, word & 31, false);
    auto& memory = system.ApplicationMemory();
    switch (size) {
    case 0:
        memory.Write8(address, static_cast<u8>(value));
        break;
    case 1:
        memory.Write16(address, static_cast<u16>(value));
        break;
    case 2:
        memory.Write32(address, static_cast<u32>(value));
        break;
    default:
        memory.Write64(address, value);
        break;
    }
}

} // namespace

bool HookInstaller::AddInline(std::size_t site_offset, Kernel::Svc::GuestHook hook,
                              const char* what) {
    const std::size_t site = text_offset + site_offset;
    if (site + 4 > text_end) {
        LOG_ERROR(Loader, "Pokopia {}: main+{:#x} is outside the text segment", what, site_offset);
        return false;
    }
    const u32 original = ReadWord(image, site);
    if (!IsPlainStore(original)) {
        LOG_ERROR(Loader, "Pokopia {}: main+{:#x} holds {:08x}, not a plain store", what,
                  site_offset, original);
        return false;
    }
    const u64 site_address = load_base + site_offset;
    InlineHooks()[site_address] = InlineHook{hook, original};
    Kernel::Svc::g_guest_hooks[site_address] = &RunInline;
    WriteWord(image, site, SvcHook);
    LOG_INFO(Loader, "Pokopia {}: hooked main+{:#x} ({:08x}) in place", what, site_offset,
             original);
    return true;
}

HookInstaller::HookInstaller(std::span<u8> image_, u64 load_base_, std::size_t text_offset_,
                             std::size_t text_size, std::size_t text_mapped)
    : image{image_}, load_base{load_base_}, text_offset{text_offset_},
      text_end{text_offset_ + text_size}, text_mapped_end{text_offset_ + text_mapped},
      next_stub{Common::AlignUp(text_offset_ + text_size, 4)} {}

bool HookInstaller::Add(std::size_t site_offset, Kernel::Svc::GuestHook hook, const char* what) {
    const std::size_t site = text_offset + site_offset;
    if (site + 4 > text_end) {
        LOG_ERROR(Loader, "Pokopia {}: main+{:#x} is outside the text segment", what, site_offset);
        return false;
    }
    const u64 site_address = load_base + site_offset;
    const std::size_t stub = next_stub;
    const u64 stub_address = load_base + (stub - text_offset);

    const u32 original = ReadWord(image, site);
    const auto moved = Relocate(original, site_address, stub_address + 4);
    if (!moved) {
        LOG_ERROR(Loader, "Pokopia {}: cannot move instruction {:08x} at main+{:#x}", what,
                  original, site_offset);
        return false;
    }
    const std::size_t stub_words = 1 + moved->size() + 1;
    const std::size_t stub_end = stub + stub_words * 4;
    if (stub_end > text_mapped_end ||
        !std::all_of(image.begin() + stub, image.begin() + stub_end,
                     [](u8 byte) { return byte == 0; })) {
        LOG_ERROR(Loader, "Pokopia {}: no free space left after the text segment", what);
        return false;
    }
    const auto back = Branch(stub_address + (stub_words - 1) * 4, site_address + 4);
    const auto to_stub = Branch(site_address, stub_address);
    if (!back || !to_stub) {
        return false;
    }

    std::size_t offset = stub;
    WriteWord(image, offset, SvcHook);
    offset += 4;
    for (const u32 word : *moved) {
        WriteWord(image, offset, word);
        offset += 4;
    }
    WriteWord(image, offset, *back);
    WriteWord(image, site, *to_stub);
    next_stub = stub_end;

    Kernel::Svc::g_guest_hooks[stub_address] = hook;
    LOG_INFO(Loader, "Pokopia {}: hooked main+{:#x} ({:08x}), stub at main+{:#x}", what,
             site_offset, original, stub - text_offset);
    return true;
}

} // namespace Core::Pokopia
