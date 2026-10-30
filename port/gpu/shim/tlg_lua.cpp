// SPDX-License-Identifier: GPL-2.0-or-later
// D2: bridge the game's HavokScript loader to stage-load completion notifications.
// The hook itself is installed by port/src/probe.c using the reference call sites.
#include "tlg_settings.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {
using LoadFn = int(__attribute__((sysv_abi)) *)(void*, void*, const char*, uint64_t, const char*);
LoadFn original = nullptr;

bool StartsWith(const char* buffer, uint64_t size, std::string_view prefix) {
    return buffer && size >= prefix.size() && std::memcmp(buffer, prefix.data(), prefix.size()) == 0;
}
} // namespace

extern "C" void tlg_lua_status(uintptr_t loadbuffer) {
    original = reinterpret_cast<LoadFn>(loadbuffer);
}

extern "C" __attribute__((sysv_abi)) int tlg_lua_load(void* L, void* options, const char* buffer,
                                                      uint64_t size, const char* name) {
    static constexpr std::string_view Empty = "--";
    if (StartsWith(buffer, size, "--TLGLOADED")) {
        TlgSettings::Get().stage_loaded.store(true, std::memory_order_relaxed);
        return original(L, options, Empty.data(), Empty.size(), name);
    }

    if (name && std::strcmp(name, "data_trico/game/scripts/common/GameSetupTbl.lua") == 0 &&
        buffer && size) {
        // Both stage transitions and checkpoint reloads end at this point. Emit the marker after
        // every matching disconnect, not merely the first one in this script.
        static constexpr std::string_view LoadEnd = "Disconnect( GameSetupTbl.timeBlocker )";
        static constexpr std::string_view Loaded =
            " do local f=(loadstring or load)('--TLGLOADED') if f then f() end end";
        std::string patched(buffer, static_cast<size_t>(size));
        bool changed = false;
        for (size_t at = patched.find(LoadEnd); at != std::string::npos;
             at = patched.find(LoadEnd, at + LoadEnd.size() + Loaded.size())) {
            patched.insert(at + LoadEnd.size(), Loaded);
            changed = true;
        }
        if (changed) {
            std::printf("Lua: marked stage-load completion in %s\n", name);
            std::fflush(stdout);
            return original(L, options, patched.data(), patched.size(), name);
        }
    }

    return original(L, options, buffer, size, name);
}
