#pragma once

#include "psx/bios.hpp"
#include "psx/cdrom.hpp"
#include "psx/cpu.hpp"
#include "psx/gte.hpp"
#include "psx/hw.hpp"
#include "psx/interpreter.hpp"
#include "psx/memory.hpp"
#include "psx/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace psx {

// Abstract peripheral sinks (stubbed in Milestone 2, implemented in Milestone 3).
class GpuSink {
public:
    virtual ~GpuSink() = default;
    virtual void reset() = 0;
    virtual void write_gp0(std::uint32_t word) = 0;
    virtual bool in_cpu_to_vram() const noexcept { return false; }
    virtual void write_gp0_words(const std::uint32_t *words, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) write_gp0(words[i]);
    }
    virtual void write_gp1(std::uint32_t word) = 0;
    virtual std::uint32_t read_data() = 0;
    virtual std::uint32_t read_stat() const = 0;
    virtual void on_vblank() {}
};

class MdecSink {
public:
    virtual ~MdecSink() = default;
    virtual void reset() = 0;
    virtual void write_cmd(std::uint32_t word) = 0;
    virtual void write_ctrl(std::uint32_t word) = 0;
    virtual std::uint32_t read_data() = 0;
    virtual std::uint32_t read_stat() const = 0;
};

class SpuSink {
public:
    virtual ~SpuSink() = default;
    virtual void reset() = 0;
    virtual std::uint16_t read_reg(std::uint32_t phys) = 0;
    virtual void write_reg(std::uint32_t phys, std::uint16_t val) = 0;
    virtual void write_dma16(std::uint16_t val) = 0;
    virtual std::uint16_t read_dma16() = 0;
    virtual void tick(std::uint32_t cycles) { (void)cycles; }
};

// AOT compiled function descriptor emitted by psxrecomp.
using AotFn = void (*)(System &sys);

struct AotEntry {
    std::uint32_t pc = 0;               // virtual PC (e.g. 0x800195BC)
    std::uint32_t byte_len = 0;         // size of function body in RAM
    std::uint64_t code_hash = 0;        // FNV-1a hash of original instruction bytes
    AotFn fn = nullptr;
    std::uint64_t verified_gen = 0;     // last per-page page_gen signature verified valid
    bool valid = false;
};

// Top-level PlayStation 1 Runtime System.
class System {
public:
    Cpu cpu;
    Gte gte;
    Memory mem;
    Hardware hw;
    Cdrom cdrom;
    Bios bios;

    GpuSink *gpu = nullptr;
    MdecSink *mdec = nullptr;
    SpuSink *spu = nullptr;

    std::uint64_t frame_count = 0;
    bool aot_enabled = true;
    std::uint64_t aot_hits = 0;
    std::uint64_t interp_fallbacks = 0;

    System();
    ~System();

    void reset();

    // Load a PS-X EXE binary into 2 MiB RAM and initialize PC/GP/SP.
    bool load_psx_exe(const std::string &path, std::string *err_out = nullptr);
    bool load_psx_exe_bytes(const std::uint8_t *data, std::size_t size, std::string *err_out = nullptr);

    // Register AOT-compiled functions from psxrecomp.
    void clear_aot();
    void register_aot(const AotEntry *entries, std::size_t count);
    AotFn lookup_aot(std::uint32_t vaddr);
    const std::uint64_t *aot_present_bitset() const noexcept;

    // Advance one full NTSC frame (263 scanlines, HBlank/VBlank IRQs, timers, CD-ROM, SPU).
    void run_frame();

    // Advance hardware peripherals by `cycles` and check pending IRQs.
    void step_hardware(std::uint32_t cycles);

    // Called at loop back-edges in AOT code to keep timers/IRQs/CD-ROM synchronized.
    inline void consume_cycles(std::uint32_t n) {
        cpu.cycles += n;
        cpu.budget -= static_cast<std::int32_t>(n);
        hw_accum_ += n;
        if (hw_accum_ >= 128u) {
            const std::uint32_t dt = hw_accum_;
            hw_accum_ = 0;
            step_hardware(dt);
        }
    }

    inline bool should_yield() const noexcept {
        return cpu.halted || cpu.budget <= 0 || cpu.irq_enabled();
    }

private:
    std::unordered_map<std::uint32_t, std::vector<AotEntry>> aot_table_;
    std::uint32_t hw_accum_ = 0;

    // Default stub sinks when external M3 GPU/MDEC/SPU are not yet attached.
    struct StubGpu;
    struct StubMdec;
    struct StubSpu;
    StubGpu *default_gpu_ = nullptr;
    StubMdec *default_mdec_ = nullptr;
    StubSpu *default_spu_ = nullptr;
};

}  // namespace psx
