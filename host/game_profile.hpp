#pragma once

// Runtime active PS2 game profile for the shared engine (engine/nocturne_ps2.exe).
// Replaces compile-time PS2_GAME_SH1 / PS2_GAME_SH3 macros so a single PS2 engine
// binary serves Shadow Hearts (1913), Covenant (1915), and From the New World (1929).

#include <cstring>
#include <string_view>

namespace nocturne {

enum class Ps2GameProfile {
    Sh1,
    Sh2,
    Sh3,
};

inline Ps2GameProfile &active_ps2_game_storage() noexcept {
    static Ps2GameProfile s_game =
#if defined(PS2_GAME_SH1)
        Ps2GameProfile::Sh1;
#elif defined(PS2_GAME_SH3)
        Ps2GameProfile::Sh3;
#else
        Ps2GameProfile::Sh2;
#endif
    return s_game;
}

inline Ps2GameProfile active_ps2_game() noexcept {
    return active_ps2_game_storage();
}

inline void set_active_ps2_game(Ps2GameProfile p) noexcept {
    active_ps2_game_storage() = p;
}

inline bool set_active_ps2_game_from_id(std::string_view id) noexcept {
    if (id == "sh1" || id == "SH1" || id == "SLUS_203.47" || id == "SLUS-20347") {
        set_active_ps2_game(Ps2GameProfile::Sh1);
        return true;
    }
    if (id == "sh2" || id == "SH2" || id == "sh2-covenant" || id == "SLUS_210.41" || id == "SLUS-21041" ||
        id == "SLUS_210.44" || id == "SLUS-21044") {
        set_active_ps2_game(Ps2GameProfile::Sh2);
        return true;
    }
    if (id == "sh3" || id == "SH3" || id == "sh3-ftnw" || id == "SLUS_213.26" || id == "SLUS-21326") {
        set_active_ps2_game(Ps2GameProfile::Sh3);
        return true;
    }
    return false;
}

inline const char *active_ps2_game_slug() noexcept {
    switch (active_ps2_game()) {
    case Ps2GameProfile::Sh1: return "sh1";
    case Ps2GameProfile::Sh3: return "sh3-ftnw";
    case Ps2GameProfile::Sh2:
    default: return "sh2-covenant";
    }
}

inline const char *active_ps2_game_title() noexcept {
    switch (active_ps2_game()) {
    case Ps2GameProfile::Sh1: return "Shadow Hearts";
    case Ps2GameProfile::Sh3: return "Shadow Hearts: From the New World";
    case Ps2GameProfile::Sh2:
    default: return "Shadow Hearts: Covenant";
    }
}

}  // namespace nocturne
