#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psx {

class System;
class Gpu;
class Mdec;
class Spu;

// Binary Save State serializer for Koudelka (slot_0.state .. slot_9.state).
bool save_state_file(const std::string &path, const System &sys, const Gpu &gpu, const Mdec &mdec, const Spu &spu);
bool load_state_file(const std::string &path, System &sys, Gpu &gpu, Mdec &mdec, Spu &spu);
bool save_state_to_memory(std::vector<std::uint8_t> &out, const System &sys, const Gpu &gpu, const Mdec &mdec, const Spu &spu);
bool load_state_from_memory(const std::uint8_t *data, std::size_t size, System &sys, Gpu &gpu, Mdec &mdec, Spu &spu);

}  // namespace psx
