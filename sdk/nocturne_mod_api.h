#pragma once

/*
 * Shadow Hearts: Nocturne — Layer 4 Native C-API v1 SDK Header
 *
 * Build a 64-bit Windows DLL exporting `NocturneModInitV1` and drop it in:
 *   games/<slug>/mods/<YourMod>/plugins/<YourPlugin>.dll
 *
 * For player safety, Layer 4 native plugins are opt-in and require
 * `allow_native_plugins = 1` in `games/<slug>/keys.ini`.
 *
 * Example build with clang++ (llvm-mingw):
 *   clang++ -shared -O2 -std=c++20 -I sdk my_plugin.cpp -o my_plugin.dll
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define NOCTURNE_MOD_EXPORT __declspec(dllexport)
#else
#define NOCTURNE_MOD_EXPORT __attribute__((visibility("default")))
#endif

#define NOCTURNE_MOD_API_VERSION 1u

typedef struct NocturneModApiV1 {
    uint32_t api_version;       /* 1 */
    const char *game_slug;      /* "koudelka", "sh1", "sh2-covenant", "sh3-ftnw" */
    uint8_t *guest_ram;         /* Base pointer to guest RAM (2 MB PS1 or 32 MB PS2 EE) */
    uint32_t guest_ram_size;

    uint8_t  (*read_u8)(uint8_t *ram, uint32_t ram_size, uint32_t addr);
    uint16_t (*read_u16)(uint8_t *ram, uint32_t ram_size, uint32_t addr);
    uint32_t (*read_u32)(uint8_t *ram, uint32_t ram_size, uint32_t addr);
    void     (*write_u8)(uint8_t *ram, uint32_t ram_size, uint32_t addr, uint8_t val);
    void     (*write_u16)(uint8_t *ram, uint32_t ram_size, uint32_t addr, uint16_t val);
    void     (*write_u32)(uint8_t *ram, uint32_t ram_size, uint32_t addr, uint32_t val);
} NocturneModApiV1;

typedef struct NocturneModCallbacksV1 {
    void (*on_boot)(const NocturneModApiV1 *api);
    void (*on_frame)(const NocturneModApiV1 *api, uint64_t frame_index);
    void (*on_battle_start)(const NocturneModApiV1 *api, uint32_t battle_id);
    void (*on_turn_begin)(const NocturneModApiV1 *api, uint32_t actor_slot);
    void (*on_ring_result)(const NocturneModApiV1 *api, uint32_t actor_slot, uint32_t hits, uint32_t perfects);
    void (*on_area_load)(const NocturneModApiV1 *api, const char *area_code);
} NocturneModCallbacksV1;

/* Required DLL entry point exported by Layer 4 native plugins. */
NOCTURNE_MOD_EXPORT bool NocturneModInitV1(const NocturneModApiV1 *api, NocturneModCallbacksV1 *out_callbacks);

#ifdef __cplusplus
}
#endif
