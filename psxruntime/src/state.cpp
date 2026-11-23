#include "psx/state.hpp"
#include "psx/gpu.hpp"
#include "psx/mdec.hpp"
#include "psx/pgxp.hpp"
#include "psx/spu.hpp"
#include "psx/system.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace psx {

namespace {

constexpr char kStateMagicV1[8] = {'P', 'S', 'X', 'S', 'T', 'A', 'T', '1'};
constexpr char kStateMagicV2[8] = {'P', 'S', 'X', 'S', 'T', 'A', 'T', '2'};

bool save_state_stream(std::ostream &ofs, const System &sys, const Gpu &gpu, const Mdec &mdec, const Spu &spu) {

    auto wr = [&](const void *p, std::size_t n) {
        ofs.write(reinterpret_cast<const char *>(p), static_cast<std::streamsize>(n));
    };

    wr(kStateMagicV2, sizeof(kStateMagicV2));
    wr(&sys.cpu, sizeof(sys.cpu));
    wr(&sys.gte, sizeof(sys.gte));
    wr(sys.mem.ram, kRamSize);
    wr(sys.mem.scratchpad, kScratchpadSize);
    wr(&sys.hw.i_stat, sizeof(sys.hw.i_stat));
    wr(&sys.hw.i_mask, sizeof(sys.hw.i_mask));
    wr(sys.hw.dma, sizeof(sys.hw.dma));
    wr(&sys.hw.dpcr, sizeof(sys.hw.dpcr));
    wr(&sys.hw.dicr, sizeof(sys.hw.dicr));
    wr(sys.hw.timers, sizeof(sys.hw.timers));

    const int disc = sys.cdrom.active_disc();
    wr(&disc, sizeof(disc));

    // GPU VRAM + display regs
    wr(gpu.vram.data(), gpu.vram.size() * sizeof(std::uint16_t));
    wr(&gpu.draw_mode, sizeof(gpu.draw_mode));
    wr(&gpu.tex_window, sizeof(gpu.tex_window));
    wr(&gpu.clip_x1, sizeof(gpu.clip_x1));
    wr(&gpu.clip_y1, sizeof(gpu.clip_y1));
    wr(&gpu.clip_x2, sizeof(gpu.clip_x2));
    wr(&gpu.clip_y2, sizeof(gpu.clip_y2));
    wr(&gpu.offset_x, sizeof(gpu.offset_x));
    wr(&gpu.offset_y, sizeof(gpu.offset_y));
    wr(&gpu.gpustat, sizeof(gpu.gpustat));
    wr(&gpu.disp_vram_x, sizeof(gpu.disp_vram_x));
    wr(&gpu.disp_vram_y, sizeof(gpu.disp_vram_y));
    wr(&gpu.disp_h1, sizeof(gpu.disp_h1));
    wr(&gpu.disp_h2, sizeof(gpu.disp_h2));
    wr(&gpu.disp_v1, sizeof(gpu.disp_v1));
    wr(&gpu.disp_v2, sizeof(gpu.disp_v2));
    wr(&gpu.disp_mode, sizeof(gpu.disp_mode));
    wr(&gpu.display_disabled, sizeof(gpu.display_disabled));
    wr(&gpu.is_24bit, sizeof(gpu.is_24bit));

    // MDEC tables
    wr(mdec.iq_y.data(), mdec.iq_y.size());
    wr(mdec.iq_uv.data(), mdec.iq_uv.size());
    wr(mdec.scale_table.data(), mdec.scale_table.size() * sizeof(std::int16_t));

    // SPU RAM + voices + regs
    wr(spu.ram.data(), spu.ram.size());
    wr(spu.voices.data(), spu.voices.size() * sizeof(Spu::Voice));
    wr(spu.regs.data(), spu.regs.size() * sizeof(std::uint16_t));
    wr(&spu.spucnt, sizeof(spu.spucnt));
    wr(&spu.transfer_addr, sizeof(spu.transfer_addr));

    // V2 extension: BIOS kernel hooks/events, CD-ROM registers, DMA delays, SPU volumes/control
    sys.bios.serialize_state(ofs);
    sys.cdrom.serialize_state(ofs);
    wr(sys.hw.dma_irq_delay, sizeof(sys.hw.dma_irq_delay));
    wr(&spu.main_vol_l, sizeof(spu.main_vol_l));
    wr(&spu.main_vol_r, sizeof(spu.main_vol_r));
    wr(&spu.cd_vol_l, sizeof(spu.cd_vol_l));
    wr(&spu.cd_vol_r, sizeof(spu.cd_vol_r));
    wr(&spu.kon, sizeof(spu.kon));
    wr(&spu.koff, sizeof(spu.koff));
    wr(&spu.endx, sizeof(spu.endx));
    wr(&spu.irq_addr, sizeof(spu.irq_addr));

    return ofs.good();
}

bool load_state_stream(std::istream &ifs, System &sys, Gpu &gpu, Mdec &mdec, Spu &spu) {
    auto rd = [&](void *p, std::size_t n) -> bool {
        ifs.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(n));
        return ifs.gcount() == static_cast<std::streamsize>(n);
    };

    char magic[8] = {};
    if (!rd(magic, sizeof(magic))) return false;
    const bool is_v2 = (std::memcmp(magic, kStateMagicV2, sizeof(kStateMagicV2)) == 0);
    const bool is_v1 = (std::memcmp(magic, kStateMagicV1, sizeof(kStateMagicV1)) == 0);
    if (!is_v1 && !is_v2) return false;

    if (!rd(&sys.cpu, sizeof(sys.cpu))) return false;
    if (!rd(&sys.gte, sizeof(sys.gte))) return false;
    if (!rd(sys.mem.ram, kRamSize)) return false;
    if (!rd(sys.mem.scratchpad, kScratchpadSize)) return false;
    sys.mem.mark_code_dirty(0, kRamSize);

    if (!rd(&sys.hw.i_stat, sizeof(sys.hw.i_stat))) return false;
    if (!rd(&sys.hw.i_mask, sizeof(sys.hw.i_mask))) return false;
    if (!rd(sys.hw.dma, sizeof(sys.hw.dma))) return false;
    if (!rd(&sys.hw.dpcr, sizeof(sys.hw.dpcr))) return false;
    if (!rd(&sys.hw.dicr, sizeof(sys.hw.dicr))) return false;
    if (!rd(sys.hw.timers, sizeof(sys.hw.timers))) return false;

    int disc = 1;
    if (!rd(&disc, sizeof(disc))) return false;
    sys.cdrom.set_active_disc_immediate(disc);

    if (!rd(gpu.vram.data(), gpu.vram.size() * sizeof(std::uint16_t))) return false;
    if (!rd(&gpu.draw_mode, sizeof(gpu.draw_mode))) return false;
    if (!rd(&gpu.tex_window, sizeof(gpu.tex_window))) return false;
    if (!rd(&gpu.clip_x1, sizeof(gpu.clip_x1))) return false;
    if (!rd(&gpu.clip_y1, sizeof(gpu.clip_y1))) return false;
    if (!rd(&gpu.clip_x2, sizeof(gpu.clip_x2))) return false;
    if (!rd(&gpu.clip_y2, sizeof(gpu.clip_y2))) return false;
    if (!rd(&gpu.offset_x, sizeof(gpu.offset_x))) return false;
    if (!rd(&gpu.offset_y, sizeof(gpu.offset_y))) return false;
    if (!rd(&gpu.gpustat, sizeof(gpu.gpustat))) return false;
    if (!rd(&gpu.disp_vram_x, sizeof(gpu.disp_vram_x))) return false;
    if (!rd(&gpu.disp_vram_y, sizeof(gpu.disp_vram_y))) return false;
    if (!rd(&gpu.disp_h1, sizeof(gpu.disp_h1))) return false;
    if (!rd(&gpu.disp_h2, sizeof(gpu.disp_h2))) return false;
    if (!rd(&gpu.disp_v1, sizeof(gpu.disp_v1))) return false;
    if (!rd(&gpu.disp_v2, sizeof(gpu.disp_v2))) return false;
    if (!rd(&gpu.disp_mode, sizeof(gpu.disp_mode))) return false;
    if (!rd(&gpu.display_disabled, sizeof(gpu.display_disabled))) return false;
    if (!rd(&gpu.is_24bit, sizeof(gpu.is_24bit))) return false;
    gpu.sync_hires_from_vram();

    if (!rd(mdec.iq_y.data(), mdec.iq_y.size())) return false;
    if (!rd(mdec.iq_uv.data(), mdec.iq_uv.size())) return false;
    if (!rd(mdec.scale_table.data(), mdec.scale_table.size() * sizeof(std::int16_t))) return false;

    if (!rd(spu.ram.data(), spu.ram.size())) return false;
    if (!rd(spu.voices.data(), spu.voices.size() * sizeof(Spu::Voice))) return false;
    if (!rd(spu.regs.data(), spu.regs.size() * sizeof(std::uint16_t))) return false;
    if (!rd(&spu.spucnt, sizeof(spu.spucnt))) return false;
    if (!rd(&spu.transfer_addr, sizeof(spu.transfer_addr))) return false;

    if (is_v2) {
        if (!sys.bios.deserialize_state(ifs)) return false;
        if (!sys.cdrom.deserialize_state(ifs)) return false;
        if (!rd(sys.hw.dma_irq_delay, sizeof(sys.hw.dma_irq_delay))) return false;
        if (!rd(&spu.main_vol_l, sizeof(spu.main_vol_l))) return false;
        if (!rd(&spu.main_vol_r, sizeof(spu.main_vol_r))) return false;
        if (!rd(&spu.cd_vol_l, sizeof(spu.cd_vol_l))) return false;
        if (!rd(&spu.cd_vol_r, sizeof(spu.cd_vol_r))) return false;
        if (!rd(&spu.kon, sizeof(spu.kon))) return false;
        if (!rd(&spu.koff, sizeof(spu.koff))) return false;
        if (!rd(&spu.endx, sizeof(spu.endx))) return false;
        if (!rd(&spu.irq_addr, sizeof(spu.irq_addr))) return false;
    }
    sys.hw.update_cpu_irq(sys);
    pgxp::clear();

    return true;
}

}  // namespace

bool save_state_file(const std::string &path, const System &sys, const Gpu &gpu, const Mdec &mdec, const Spu &spu) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    return save_state_stream(ofs, sys, gpu, mdec, spu);
}

bool load_state_file(const std::string &path, System &sys, Gpu &gpu, Mdec &mdec, Spu &spu) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    return load_state_stream(ifs, sys, gpu, mdec, spu);
}

bool save_state_to_memory(std::vector<std::uint8_t> &out, const System &sys, const Gpu &gpu, const Mdec &mdec, const Spu &spu) {
    std::ostringstream oss(std::ios::binary);
    if (!save_state_stream(oss, sys, gpu, mdec, spu)) return false;
    const std::string bytes = oss.str();
    out.assign(bytes.begin(), bytes.end());
    return !out.empty();
}

bool load_state_from_memory(const std::uint8_t *data, std::size_t size, System &sys, Gpu &gpu, Mdec &mdec, Spu &spu) {
    if (!data || size == 0) return false;
    std::string buf(reinterpret_cast<const char *>(data), size);
    std::istringstream iss(buf, std::ios::binary);
    return load_state_stream(iss, sys, gpu, mdec, spu);
}

}  // namespace psx
