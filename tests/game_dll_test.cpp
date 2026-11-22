#include "game_profile.hpp"
#include "ps2/game_dll_abi.hpp"
#include "psx/game_dll_abi.hpp"

#include <cstdio>

int main() {
    if (ps2::kPs2GameAbiVersion == 0u || ps2::ps2_game_dll_layout() == 0ull) {
        std::fprintf(stderr, "FAIL: invalid PS2 game DLL ABI/layout\n");
        return 1;
    }
    if (psx::kPsxGameAbiVersion == 0u || psx::psx_game_dll_layout() == 0ull) {
        std::fprintf(stderr, "FAIL: invalid PSX game DLL ABI/layout\n");
        return 1;
    }

    if (!nocturne::set_active_ps2_game_from_id("sh1") ||
        nocturne::active_ps2_game() != nocturne::Ps2GameProfile::Sh1 ||
        std::string_view(nocturne::active_ps2_game_slug()) != "sh1") {
        std::fprintf(stderr, "FAIL: SH1 game profile mapping\n");
        return 1;
    }
    if (!nocturne::set_active_ps2_game_from_id("SLUS_210.41") ||
        nocturne::active_ps2_game() != nocturne::Ps2GameProfile::Sh2 ||
        std::string_view(nocturne::active_ps2_game_slug()) != "sh2-covenant") {
        std::fprintf(stderr, "FAIL: SH2 game profile mapping\n");
        return 1;
    }
    if (!nocturne::set_active_ps2_game_from_id("SLUS_213.26") ||
        nocturne::active_ps2_game() != nocturne::Ps2GameProfile::Sh3 ||
        std::string_view(nocturne::active_ps2_game_slug()) != "sh3-ftnw") {
        std::fprintf(stderr, "FAIL: SH3 game profile mapping\n");
        return 1;
    }
    if (nocturne::set_active_ps2_game_from_id("unknown-game")) {
        std::fprintf(stderr, "FAIL: unknown game ID should return false\n");
        return 1;
    }

    std::printf("PASS: game_dll_test (PS2 ABI v%u layout=0x%016llX, PSX ABI v%u layout=0x%016llX)\n",
                static_cast<unsigned>(ps2::kPs2GameAbiVersion),
                static_cast<unsigned long long>(ps2::ps2_game_dll_layout()),
                static_cast<unsigned>(psx::kPsxGameAbiVersion),
                static_cast<unsigned long long>(psx::psx_game_dll_layout()));
    return 0;
}
