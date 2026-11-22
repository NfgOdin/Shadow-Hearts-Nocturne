#include "psx/system.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace psx {

// Minimal Milestone 2 default stubs (replaced by Milestone 3 GPU/MDEC/SPU1 implementations).
struct System::StubGpu final : GpuSink {
    std::uint32_t stat = 0x1C000000u;  // Ready for commands, DMA, VRAM->CPU
    std::uint32_t last_read = 0;
    void reset() override { stat = 0x1C000000u; last_read = 0; }
    void write_gp0(std::uint32_t) override {}
    void write_gp1(std::uint32_t w) override {
        const std::uint32_t cmd = w >> 24;
        if (cmd == 0x00u) { stat = 0x1C000000u; }
        else if (cmd == 0x03u) { stat = (stat & ~(1u << 23)) | ((w & 1u) << 23); }
        else if (cmd == 0x04u) { stat = (stat & ~(3u << 29)) | ((w & 3u) << 29); }
        else if (cmd == 0x08u) {
            stat = (stat & ~0x007F0000u) |
                   ((w & 0x3Fu) << 17) | (((w >> 6) & 1u) << 16);
        }
    }
    std::uint32_t read_data() override { return last_read; }
    std::uint32_t read_stat() const override { return stat; }
};

struct System::StubMdec final : MdecSink {
    void reset() override {}
    void write_cmd(std::uint32_t) override {}
    void write_ctrl(std::uint32_t) override {}
    std::uint32_t read_data() override { return 0u; }
    std::uint32_t read_stat() const override { return 0x80040000u; }
};

struct System::StubSpu final : SpuSink {
    std::uint16_t regs[0x200] = {};
    void reset() override { std::memset(regs, 0, sizeof(regs)); }
    std::uint16_t read_reg(std::uint32_t phys) override {
        const std::uint32_t off = (phys - 0x1F801C00u) >> 1;
        if (phys == 0x1F801DAEu) return regs[0x1AA >> 1] & 0x003Fu;  // SPUSTAT mirrors SPUCNT low 6 bits
        return (off < 0x200u) ? regs[off] : 0u;
    }
    void write_reg(std::uint32_t phys, std::uint16_t val) override {
        const std::uint32_t off = (phys - 0x1F801C00u) >> 1;
        if (off < 0x200u) regs[off] = val;
    }
    void write_dma16(std::uint16_t) override {}
    std::uint16_t read_dma16() override { return 0u; }
};

System::System() {
    default_gpu_ = new StubGpu();
    default_mdec_ = new StubMdec();
    default_spu_ = new StubSpu();
    gpu = default_gpu_;
    mdec = default_mdec_;
    spu = default_spu_;
    reset();
}

System::~System() {
    delete default_gpu_;
    delete default_mdec_;
    delete default_spu_;
}

void System::reset() {
    cpu.reset();
    gte.reset();
    mem.reset();
    hw.reset();
    cdrom.reset();
    if (gpu) gpu->reset();
    if (mdec) mdec->reset();
    if (spu) spu->reset();
    bios.reset(*this);
    frame_count = 0;
    aot_hits = 0;
    interp_fallbacks = 0;
    hw_accum_ = 0;
    for (auto &[pc, entries] : aot_table_) {
        for (auto &entry : entries) {
            entry.verified_gen = 0;
            entry.valid = false;
        }
    }
}

bool System::load_psx_exe(const std::string &path, std::string *err_out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        if (err_out) *err_out = "Cannot open PS-X EXE: " + path;
        return false;
    }
    const std::streamsize sz = ifs.tellg();
    if (sz < 0x800) {
        if (err_out) *err_out = "File too small to be a PS-X EXE (<2048 bytes)";
        return false;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char *>(buf.data()), sz);
    return load_psx_exe_bytes(buf.data(), buf.size(), err_out);
}

bool System::load_psx_exe_bytes(const std::uint8_t *data, std::size_t size, std::string *err_out) {
    if (!data || size < 0x800 || std::memcmp(data, "PS-X EXE", 8) != 0) {
        if (err_out) *err_out = "Invalid PS-X EXE header magic";
        return false;
    }

    auto u32 = [&](std::size_t off) -> std::uint32_t {
        std::uint32_t v = 0;
        std::memcpy(&v, data + off, 4);
        return v;
    };

    const std::uint32_t init_pc = u32(0x10);
    const std::uint32_t init_gp = u32(0x14);
    const std::uint32_t load_addr = u32(0x18);
    const std::uint32_t text_size = u32(0x1C);
    const std::uint32_t bss_addr = u32(0x28);
    const std::uint32_t bss_size = u32(0x2C);
    const std::uint32_t sp_base = u32(0x30);
    const std::uint32_t sp_off = u32(0x34);

    if (0x800u + text_size > size) {
        if (err_out) *err_out = "PS-X EXE text_size exceeds file size";
        return false;
    }

    const std::uint32_t phys_load = virt_to_phys(load_addr);
    if (phys_load >= kRamSize || phys_load + text_size > kRamSize) {
        if (err_out) *err_out = "PS-X EXE load range exceeds 2 MiB PS1 RAM";
        return false;
    }

    mem.write_ram_bytes(phys_load, data + 0x800, text_size);

    if (bss_size > 0 && virt_to_phys(bss_addr) + bss_size <= kRamSize) {
        std::memset(mem.ram + (virt_to_phys(bss_addr) & kRamMask), 0, bss_size);
    }

    cpu.pc = init_pc;
    cpu.npc = init_pc + 4u;
    cpu.r[28] = init_gp;  // $gp
    const std::uint32_t sp = (sp_base != 0) ? (sp_base + sp_off) : 0x801FFFF0u;
    cpu.r[29] = sp;       // $sp
    cpu.r[30] = sp;       // $fp
    // Enable COP0/COP2 and hardware IRQ mask IM2 + IEc (standard PS1 state upon EXE launch)
    cpu.cop0[kCop0Sr] = 0x10000401u;

    return true;
}

void System::clear_aot() {
    aot_table_.clear();
}

void System::register_aot(const AotEntry *entries, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        aot_table_[entries[i].pc].push_back(entries[i]);
    }
}

AotFn System::lookup_aot(std::uint32_t vaddr) {
    auto it = aot_table_.find(vaddr);
    if (it == aot_table_.end()) return nullptr;
    auto &vec = it->second;
    const std::uint32_t phys = virt_to_phys(vaddr);

    for (AotEntry &e : vec) {
        if (!e.fn || e.byte_len == 0) continue;
        if (phys + e.byte_len > kRamSize) {
            e.valid = false;
            continue;
        }
        const std::uint32_t start_p = (phys & kRamMask) >> kCodePageShift;
        const std::uint32_t end_p = ((phys + e.byte_len - 1u) & kRamMask) >> kCodePageShift;
        std::uint64_t page_sig = 0x9E3779B97F4A7C15ull;
        for (std::uint32_t p = start_p; p <= end_p; ++p) {
            page_sig = (page_sig * 0xBF58476D1CE4E5B9ull) ^ (static_cast<std::uint64_t>(mem.page_gen[p]) + 1u);
        }
        if (page_sig == 0) page_sig = 1;
        if (e.verified_gen != page_sig) {
            const std::uint64_t live_hash = fnv1a_words(mem.ram + phys, e.byte_len);
            e.valid = (live_hash == e.code_hash);
            e.verified_gen = page_sig;
        }
        if (e.valid) return e.fn;
    }
    return nullptr;
}

void System::step_hardware(std::uint32_t cycles) {
    if (cycles == 0) return;
    hw.tick(*this, cycles);
    cdrom.tick(*this, cycles);
    if (spu) spu->tick(cycles);
}

void System::run_frame() {
    for (std::uint32_t line = 0; line < kScanlinesNtsc && !cpu.halted; ++line) {
        cpu.budget = static_cast<std::int32_t>(kCyclesPerScanline);
        run_interpreter(*this, 0u);
        hw.on_hblank(*this);

        if (line == 240u) {
            bios.on_vblank(*this);
            hw.on_vblank(*this);
            if (gpu) gpu->on_vblank();
        }
    }
    ++frame_count;
}

}  // namespace psx
