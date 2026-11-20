#pragma once

#include "ps2/video.hpp"
#include <cstdint>
#include <cstddef>

namespace ps2::state {
class Stream;
}

namespace ps2::ipu {

void state(ps2::state::Stream &s);

void reset();
std::uint32_t read32(std::uint32_t a);
std::uint64_t read64(std::uint32_t a);  // read32(a) and read32(a + 4) in one call
void write32(std::uint32_t a, std::uint32_t v);
void write_fifo(const std::uint8_t *qword);
void read_fifo(std::uint8_t *qword);
void dma_started(int channel);
void stats();

// Commands that decode or convert a picture (IDEC, BDEC, VDEC, FDEC, CSC) given since the start: the picture pace reads it once a
// refresh to see that a film is playing. Not part of a save state.
std::uint64_t decode_commands();

// Deterministic MPEG-2 FMV stream identification, frame counter, and optional .m2v dumper for HD FMV replacement.
std::uint64_t hash_mpeg2_prefix(const std::uint8_t *bytes, std::size_t len);
void observe_mpeg2_bytes(const std::uint8_t *bytes, std::size_t len);
void on_fmv_idle();
ps2::FmvStreamInfo fmv_stream_info();
void set_fmv_dump_enabled(bool on, const char *dumps_dir = nullptr);
bool fmv_dump_enabled();

}
