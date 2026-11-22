// Entry point and runtime forwarders compiled into games/koudelka/game.dll.
// Compiled alongside generated/koudelka/unit_*.cpp and registry.cpp with zero
// linkage to psxruntime or SDL2.

#include "psx/game_dll_abi.hpp"

namespace psx_gen {
void register_all(psx::System &sys);
}

namespace psx {

static const PsxGameHost *g_psx_game_host = nullptr;

std::uint8_t Memory::read8(System &sys, std::uint32_t vaddr) const {
    return g_psx_game_host->mem_read8(this, sys, vaddr);
}

std::uint16_t Memory::read16(System &sys, std::uint32_t vaddr) const {
    return g_psx_game_host->mem_read16(this, sys, vaddr);
}

std::uint32_t Memory::read32(System &sys, std::uint32_t vaddr) const {
    return g_psx_game_host->mem_read32(this, sys, vaddr);
}

void Memory::write8(System &sys, std::uint32_t vaddr, std::uint8_t val) {
    g_psx_game_host->mem_write8(this, sys, vaddr, val);
}

void Memory::write16(System &sys, std::uint32_t vaddr, std::uint16_t val) {
    g_psx_game_host->mem_write16(this, sys, vaddr, val);
}

void Memory::write32(System &sys, std::uint32_t vaddr, std::uint32_t val) {
    g_psx_game_host->mem_write32(this, sys, vaddr, val);
}

std::uint32_t Gte::mfc2(std::uint32_t reg) const {
    return g_psx_game_host->gte_mfc2(this, reg);
}

void Gte::mtc2(std::uint32_t reg, std::uint32_t val) {
    g_psx_game_host->gte_mtc2(this, reg, val);
}

std::uint32_t Gte::cfc2(std::uint32_t reg) const {
    return g_psx_game_host->gte_cfc2(this, reg);
}

void Gte::ctc2(std::uint32_t reg, std::uint32_t val) {
    g_psx_game_host->gte_ctc2(this, reg, val);
}

void Gte::execute(std::uint32_t cmd) {
    g_psx_game_host->gte_execute(this, cmd);
}

void Hardware::update_cpu_irq(System &sys) {
    g_psx_game_host->hw_update_cpu_irq(this, sys);
}

bool Bios::handle_vector(System &sys, std::uint32_t vector_addr) {
    return g_psx_game_host->bios_handle_vector(this, sys, vector_addr);
}

void Bios::handle_syscall(System &sys) {
    g_psx_game_host->bios_handle_syscall(this, sys);
}

void System::step_hardware(std::uint32_t cycles) {
    g_psx_game_host->sys_step_hardware(this, cycles);
}

void System::register_aot(const AotEntry *entries, std::size_t count) {
    g_psx_game_host->sys_register_aot(this, entries, count);
}

void dispatch(System &sys, std::uint32_t target_pc) {
    g_psx_game_host->dispatch_fn(sys, target_pc);
}

}  // namespace psx

#if defined(_WIN32)
#define NOCTURNE_PSX_EXPORT extern "C" __declspec(dllexport)
#else
#define NOCTURNE_PSX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

NOCTURNE_PSX_EXPORT std::uint32_t nocturne_psx_game_abi() {
    return psx::kPsxGameAbiVersion;
}

NOCTURNE_PSX_EXPORT std::uint64_t nocturne_psx_game_layout() {
    return psx::psx_game_dll_layout();
}

NOCTURNE_PSX_EXPORT bool nocturne_psx_game_init(const psx::PsxGameHost *host, psx::System *sys) {
    if (!host || !sys) return false;
    if (host->abi_version != psx::kPsxGameAbiVersion) return false;
    if (host->layout != psx::psx_game_dll_layout()) return false;
    psx::g_psx_game_host = host;
    psx_gen::register_all(*sys);
    return true;
}
