#include "psx_exe.hpp"

#include <cstring>
#include <fstream>

namespace psxrecomp {

namespace {

std::uint32_t rd32(const std::uint8_t *p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::string basename_only(const std::string &path) {
    const std::size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

}  // namespace

bool load_psx_exe(const std::string &path, PsxBinary &out, std::string &err) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        err = "Cannot open PS-X EXE: " + path;
        return false;
    }
    const std::streamsize sz = ifs.tellg();
    if (sz < 0x800) {
        err = "File smaller than 2048-byte PS-X EXE header";
        return false;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char *>(raw.data()), sz);
    if (ifs.gcount() != sz) {
        err = "Failed to read complete PS-X EXE: " + path;
        return false;
    }

    if (std::memcmp(raw.data(), "PS-X EXE", 8) != 0) {
        err = "Invalid PS-X EXE magic";
        return false;
    }

    out = PsxBinary{};
    out.name = basename_only(path);
    out.entry_pc = rd32(raw.data() + 0x10);
    out.init_gp = rd32(raw.data() + 0x14);
    out.load_addr = rd32(raw.data() + 0x18);
    out.text_size = rd32(raw.data() + 0x1C);
    if (static_cast<std::uint64_t>(0x800u) + out.text_size > raw.size()) {
        err = "PS-X EXE text_size exceeds file size";
        return false;
    }
    out.bytes.assign(raw.begin() + 0x800, raw.begin() + 0x800 + out.text_size);
    return true;
}

bool load_raw_overlay(const std::string &path, std::uint32_t base_addr, PsxBinary &out, std::string &err) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        err = "Cannot open overlay binary: " + path;
        return false;
    }
    const std::streamsize sz = ifs.tellg();
    if (sz < 4 || sz > 0x200000) {
        err = (sz < 4) ? "Overlay binary too small" : "Overlay binary exceeds 2 MiB PS1 RAM limit";
        return false;
    }
    out = PsxBinary{};
    out.name = basename_only(path);
    out.entry_pc = base_addr;
    out.load_addr = base_addr;
    out.text_size = static_cast<std::uint32_t>(sz & ~3);
    out.bytes.resize(out.text_size);
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char *>(out.bytes.data()), out.text_size);
    if (ifs.gcount() != static_cast<std::streamsize>(out.text_size)) {
        err = "Failed to read complete overlay binary: " + path;
        return false;
    }
    return true;
}

}  // namespace psxrecomp
