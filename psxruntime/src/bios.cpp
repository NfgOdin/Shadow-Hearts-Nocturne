#include "psx/bios.hpp"
#include "psx/interpreter.hpp"
#include "psx/system.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <ostream>

namespace psx {

namespace {

constexpr std::uint32_t kEventStatusDisabled = 0x1000u;
constexpr std::uint32_t kEventStatusEnabled = 0x2000u;
constexpr std::uint32_t kEventStatusReady = 0x4000u;

constexpr std::uint32_t kEventModeCallback = 0x1000u;

// Standard PS1 BIOS Event classes
constexpr std::uint32_t kEvClassVBlank = 0xF0000001u;
constexpr std::uint32_t kEvClassGpu    = 0xF0000002u;
constexpr std::uint32_t kEvClassCdrom  = 0xF0000003u;
constexpr std::uint32_t kEvClassDma    = 0xF0000004u;
constexpr std::uint32_t kEvClassRcnt0  = 0xF0000005u;
constexpr std::uint32_t kEvClassSpu    = 0xF0000009u;
constexpr std::uint32_t kEvClassSwCard = 0xF4000001u;
constexpr std::uint32_t kEvClassHwCard = 0xF0000011u;

constexpr std::uint32_t kEvSpecIoEnd = 0x0004u;
constexpr std::uint32_t kEvSpecIrq   = 0x0002u;
constexpr std::uint32_t kEvSpecNew   = 0x2000u;

inline std::uint32_t make_event_handle(std::uint32_t idx) { return 0xF1000000u | idx; }
inline int event_index(std::uint32_t handle) {
    if ((handle & 0xFFFF0000u) != 0xF1000000u) return -1;
    const std::uint32_t idx = handle & 0xFFFFu;
    return (idx < Bios::kMaxEvents) ? static_cast<int>(idx) : -1;
}

}  // namespace

void Bios::reset(System &sys) {
    events_.fill(Event{});
    fds_.fill(FileHandle{});
    dir_search_ = DirSearch{};
    for (auto &h : intr_heads_) h = 0;
    custom_exit_buf_ = 0;
    pad_buf_[0] = pad_buf_[1] = 0;
    pad_len_[0] = pad_len_[1] = 0;
    pad_started_ = false;
    clear_pad_ = false;
    for (auto &c : clear_rcnt_) c = true;
    card_started_ = false;
    card_shared_irq_ = false;
    card_last_status_[0] = card_last_status_[1] = 0;
    heap_base_ = heap_cur_ = heap_end_ = 0;
    current_thread_ = 0;
    in_irq_handler_ = false;
    stdout_buf_.clear();
    install_bios_stubs(sys);
}

void Bios::install_bios_stubs(System &sys) {
    // Write recognizable BIOS version string into BIOS ROM (PsyQ checks 0xBFC7FF32 / 0xBFC7FF45)
    static constexpr const char kBiosDate[] = "1995-12-04";
    static constexpr const char kBiosVer[] = "CEX-3000/1001/1002 by K.S.";
    std::memcpy(sys.mem.bios + 0x7FF32u, kBiosDate, sizeof(kBiosDate));
    std::memcpy(sys.mem.bios + 0x7FF45u, kBiosVer, sizeof(kBiosVer));

    // Populate low-RAM vector instructions (`jr $ra; nop`) at 0xA0/0xB0/0xC0/0x80 so any inspection sees valid MIPS
    auto put32 = [&](std::uint32_t addr, std::uint32_t w) {
        std::memcpy(sys.mem.ram + (addr & kRamMask), &w, 4);
    };
    for (std::uint32_t vec : {0x80u, 0xA0u, 0xB0u, 0xC0u}) {
        put32(vec, 0x03E00008u);      // jr $ra
        put32(vec + 4u, 0x00000000u); // nop
    }

    // Kernel pointer table at 0x80000100 (PsyQ GetTcb reads [0x80000108])
    put32(0x100u, 0x80001000u);
    put32(0x104u, 0x00000004u);
    put32(0x108u, kTcbArrayAddr);
    put32(0x10Cu, 0x00000C00u);
    // Current TCB pointer at 0x80000108[0]
    put32(kTcbArrayAddr, kTcbArrayAddr + 0xC0u);
}

std::string Bios::read_guest_cstr(System &sys, std::uint32_t addr, std::size_t max_len) {
    std::string s;
    s.reserve(64);
    for (std::size_t i = 0; i < max_len; ++i) {
        const std::uint8_t b = sys.mem.read8(sys, addr + static_cast<std::uint32_t>(i));
        if (b == 0) break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

std::uint32_t Bios::open_event(std::uint32_t class_id, std::uint32_t spec, std::uint32_t mode, std::uint32_t func) {
    for (std::uint32_t i = 0; i < kMaxEvents; ++i) {
        if (!events_[i].used) {
            events_[i].used = true;
            events_[i].class_id = class_id;
            events_[i].spec = spec;
            events_[i].mode = mode;
            events_[i].func = func;
            events_[i].status = kEventStatusDisabled;
            return make_event_handle(i);
        }
    }
    return 0xFFFFFFFFu;
}

bool Bios::close_event(std::uint32_t handle) {
    const int idx = event_index(handle);
    if (idx < 0 || !events_[idx].used) return false;
    events_[idx] = Event{};
    return true;
}

bool Bios::enable_event(std::uint32_t handle) {
    const int idx = event_index(handle);
    if (idx < 0 || !events_[idx].used) return false;
    events_[idx].status = kEventStatusEnabled;
    return true;
}

bool Bios::disable_event(std::uint32_t handle) {
    const int idx = event_index(handle);
    if (idx < 0 || !events_[idx].used) return false;
    events_[idx].status = kEventStatusDisabled;
    return true;
}

bool Bios::test_event(std::uint32_t handle) {
    const int idx = event_index(handle);
    if (idx < 0 || !events_[idx].used) return false;
    if (events_[idx].status == kEventStatusReady) {
        events_[idx].status = kEventStatusEnabled;
        return true;
    }
    return false;
}

bool Bios::wait_event(System &sys, std::uint32_t handle) {
    const int idx = event_index(handle);
    if (idx < 0 || !events_[idx].used) return false;
    for (int iter = 0; iter < 512; ++iter) {
        if (events_[idx].status == kEventStatusReady) {
            events_[idx].status = kEventStatusEnabled;
            return true;
        }
        sys.step_hardware(128u);
        sys.cpu.cycles += 128u;
        sys.cpu.budget -= 128;
        if (sys.cpu.irq_enabled()) {
            handle_irq_exception(sys);
        }
    }
    events_[idx].status = kEventStatusEnabled;
    return true;
}

void Bios::deliver_event(System &sys, std::uint32_t class_id, std::uint32_t spec) {
    for (auto &ev : events_) {
        if (!ev.used) continue;
        if (ev.class_id != class_id || ev.spec != spec) continue;
        if (ev.status != kEventStatusEnabled) continue;
        if (ev.mode == kEventModeCallback && ev.func != 0) {
            invoke_guest_callback(sys, ev.func);
        } else {
            ev.status = kEventStatusReady;
        }
    }
}

void Bios::undeliver_event(std::uint32_t class_id, std::uint32_t spec) {
    for (auto &ev : events_) {
        if (!ev.used) continue;
        if (ev.class_id == class_id && ev.spec == spec && ev.status == kEventStatusReady) {
            ev.status = kEventStatusEnabled;
        }
    }
}

void Bios::sys_enq_int_rp(System &sys, std::uint32_t priority, std::uint32_t node_addr) {
    if (priority >= 4u || node_addr == 0) return;
    // Check if already enqueued
    std::uint32_t cur = intr_heads_[priority];
    for (int guard = 0; cur != 0 && guard < 32; ++guard) {
        if (cur == node_addr) return;
        cur = sys.mem.read32(sys, cur);
    }
    // Insert at head: node->next = head; head = node
    sys.mem.write32(sys, node_addr, intr_heads_[priority]);
    intr_heads_[priority] = node_addr;
}

void Bios::sys_deq_int_rp(System &sys, std::uint32_t priority, std::uint32_t node_addr) {
    if (priority >= 4u || node_addr == 0) return;
    std::uint32_t prev = 0;
    std::uint32_t cur = intr_heads_[priority];
    for (int guard = 0; cur != 0 && guard < 32; ++guard) {
        const std::uint32_t next = sys.mem.read32(sys, cur);
        if (cur == node_addr) {
            if (prev == 0) intr_heads_[priority] = next;
            else sys.mem.write32(sys, prev, next);
            return;
        }
        prev = cur;
        cur = next;
    }
}

void Bios::invoke_guest_callback(System &sys, std::uint32_t func_pc, std::uint32_t a0, std::uint32_t a1) {
    if (func_pc == 0) return;
    // Save caller registers across nested callback invocation
    const Cpu saved = sys.cpu;
    sys.cpu.flush_load_delay();
    sys.cpu.r[4] = a0;
    sys.cpu.r[5] = a1;
    // Disable nested IRQs inside callback execution
    sys.cpu.cop0[kCop0Sr] &= ~1u;
    call_guest_function(sys, func_pc, 20000u);
    const std::uint64_t spent = sys.cpu.cycles - saved.cycles;
    sys.cpu = saved;
    sys.cpu.cycles += spent;
    sys.cpu.budget -= static_cast<std::int32_t>(spent);
}

void Bios::handle_syscall(System &sys) {
    const std::uint32_t op = sys.cpu.r[4];
    switch (op) {
    case 1u: {  // EnterCriticalSection: disable IRQs, return previous (IEc && IM2) != 0
        const std::uint32_t sr = sys.cpu.cop0[kCop0Sr];
        const bool was_enabled = (sr & 0x0401u) == 0x0401u;
        sys.cpu.cop0[kCop0Sr] = sr & ~0x0401u;
        sys.cpu.r[2] = was_enabled ? 1u : 0u;
        break;
    }
    case 2u:  // ExitCriticalSection: enable IRQs (IEc + IM2)
        sys.cpu.cop0[kCop0Sr] |= 0x0401u;
        break;
    case 3u:  // ChangeThreadSubFunction
    default:
        break;
    }
}

void Bios::handle_irq_exception(System &sys) {
    if (in_irq_handler_) return;
    in_irq_handler_ = true;

    // Save full CPU context to allow clean restoration on ReturnFromException
    const Cpu saved = sys.cpu;
    sys.cpu.flush_load_delay();
    // Push IE/KU stack bits in SR (disable interrupts while in handler)
    const std::uint32_t sr = sys.cpu.cop0[kCop0Sr];
    sys.cpu.cop0[kCop0Sr] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    sys.cpu.cop0[kCop0Epc] = saved.pc;

    // 1. Run PsyQ / game SysEnqIntRP interrupt chains (priority 0 -> 3)
    //    Each node: [0]=next, [4]=func2(handler), [8]=func1(verifier), [12]=pad
    for (int prio = 0; prio < 4; ++prio) {
        std::uint32_t node = intr_heads_[prio];
        for (int guard = 0; node != 0 && guard < 32; ++guard) {
            const std::uint32_t next = sys.mem.read32(sys, node);
            const std::uint32_t func2 = sys.mem.read32(sys, node + 4u);
            const std::uint32_t func1 = sys.mem.read32(sys, node + 8u);
            bool run_func2 = true;
            if (func1 != 0) {
                const Cpu pre = sys.cpu;
                call_guest_function(sys, func1, 200000u);
                run_func2 = (sys.cpu.r[2] != 0);
                const std::uint64_t spent = sys.cpu.cycles - pre.cycles;
                sys.cpu = pre;
                sys.cpu.cycles += spent;
            }
            if (run_func2 && func2 != 0) {
                const Cpu pre = sys.cpu;
                call_guest_function(sys, func2, 500000u);
                const std::uint64_t spent = sys.cpu.cycles - pre.cycles;
                sys.cpu = pre;
                sys.cpu.cycles += spent;
            }
            node = next;
        }
    }

    // 2. Deliver BIOS events for any active hardware IRQ bits (before custom_exit_buf_ clears I_STAT)
    const std::uint32_t active = sys.hw.i_stat & sys.hw.i_mask;
    if (active & (1u << kIrqVBlank)) {
        deliver_event(sys, kEvClassVBlank, kEvSpecIrq);
        deliver_event(sys, 0xF2000003u, kEvSpecIrq);  // RCntCNT3 (VBlank root counter)
    }
    if (active & (1u << kIrqGpu)) {
        deliver_event(sys, kEvClassGpu, kEvSpecIrq);
    }
    if (active & (1u << kIrqCdrom)) {
        deliver_event(sys, kEvClassCdrom, kEvSpecIrq);
    }
    if (active & (1u << kIrqDma)) {
        deliver_event(sys, kEvClassDma, kEvSpecIrq);
    }
    for (int t = 0; t < 3; ++t) {
        if (active & (1u << (kIrqTimer0 + t))) {
            deliver_event(sys, kEvClassRcnt0 + static_cast<std::uint32_t>(t), kEvSpecIrq);
            deliver_event(sys, 0xF2000000u + static_cast<std::uint32_t>(t), kEvSpecIrq);  // RCntCNT0..2
        }
    }
    if (active & (1u << kIrqSpu)) {
        deliver_event(sys, kEvClassSpu, kEvSpecIrq);
    }

    // 3. If SetCustomExitFromException (B0:19) was registered (PsyQ ResetCallback / libetc master IRQ dispatcher),
    //    restore its setjmp buffer (ra=+0x00, sp=+0x04, fp=+0x08, s0..s7=+0x0C..0x28, gp=+0x2C) with $v0 = 1
    //    and execute until it calls ReturnFromException (B0:17).
    if (custom_exit_buf_ != 0) {
        const Cpu pre = sys.cpu;
        const std::uint32_t jb = custom_exit_buf_;
        const std::uint32_t hook_pc = sys.mem.read32(sys, jb + 0x00u);
        if (hook_pc != 0) {
            sys.cpu.r[2]  = 1u;  // $v0 = 1 (longjmp return value)
            sys.cpu.r[29] = sys.mem.read32(sys, jb + 0x04u);  // $sp
            sys.cpu.r[30] = sys.mem.read32(sys, jb + 0x08u);  // $fp
            for (int s = 0; s < 8; ++s) {
                sys.cpu.r[16 + s] = sys.mem.read32(sys, jb + 0x0Cu + static_cast<std::uint32_t>(s * 4));
            }
            sys.cpu.r[28] = sys.mem.read32(sys, jb + 0x2Cu);  // $gp
            call_guest_function(sys, hook_pc, 100000u);
            const std::uint64_t spent = sys.cpu.cycles - pre.cycles;
            sys.cpu = pre;
            sys.cpu.cycles += spent;
        }
    }

    // 4. Clear only the bits that were active at entry and not re-raised by a new transfer
    sys.hw.i_stat &= ~active;
    if ((sys.hw.dicr & (1u << 31)) != 0) {
        sys.hw.i_stat |= (1u << kIrqDma);
    }

    // Restore CPU state cleanly, update COP0.CAUSE IP2 from current i_stat & i_mask, and deduct spent cycles
    const std::uint64_t spent = sys.cpu.cycles - saved.cycles;
    sys.cpu = saved;
    sys.cpu.cycles += spent;
    sys.cpu.budget -= static_cast<std::int32_t>(spent);
    sys.hw.update_cpu_irq(sys);
    in_irq_handler_ = false;
}

void Bios::on_vblank(System &sys) {
    // Update BIOS InitPAD/StartPAD buffers directly on every VBlank
    if (pad_started_) {
        for (int p = 0; p < 2; ++p) {
            if (pad_buf_[p] != 0 && pad_len_[p] >= 4u) {
                const PadState &ps = sys.hw.pads[p];
                if (!ps.connected) {
                    sys.mem.write8(sys, pad_buf_[p] + 0u, 0xFFu);
                    sys.mem.write8(sys, pad_buf_[p] + 1u, 0xFFu);
                } else {
                    sys.mem.write8(sys, pad_buf_[p] + 0u, 0x00u);  // Status: OK
                    sys.mem.write8(sys, pad_buf_[p] + 1u, ps.analog_mode ? 0x73u : 0x41u);
                    sys.mem.write8(sys, pad_buf_[p] + 2u, static_cast<std::uint8_t>(ps.buttons & 0xFFu));
                    sys.mem.write8(sys, pad_buf_[p] + 3u, static_cast<std::uint8_t>(ps.buttons >> 8));
                    if (ps.analog_mode && pad_len_[p] >= 8u) {
                        sys.mem.write8(sys, pad_buf_[p] + 4u, ps.rx);
                        sys.mem.write8(sys, pad_buf_[p] + 5u, ps.ry);
                        sys.mem.write8(sys, pad_buf_[p] + 6u, ps.lx);
                        sys.mem.write8(sys, pad_buf_[p] + 7u, ps.ly);
                    }
                }
            }
        }
    }
}

// ---------------- Memory Card bu00:/bu10: filesystem ----------------

int Bios::parse_card_path(const std::string &path, std::string &fname_out) const {
    if (path.size() < 5) return -1;
    std::string prefix = path.substr(0, 5);
    for (auto &c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int slot = -1;
    if (prefix == "bu00:") slot = 0;
    else if (prefix == "bu10:") slot = 1;
    else return -1;
    fname_out = path.substr(5);
    while (!fname_out.empty() && (fname_out.front() == '/' || fname_out.front() == '\\')) {
        fname_out.erase(fname_out.begin());
    }
    return slot;
}

bool Bios::wildcard_match(const std::string &pat, const std::string &str) {
    std::size_t pi = 0, si = 0, star = std::string::npos, match = 0;
    while (si < str.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == str[si])) {
            ++pi; ++si;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            match = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

int Bios::find_card_file(const System &sys, int slot, const std::string &fname) const {
    if (slot < 0 || slot > 1 || !sys.hw.memcards[slot].present) return -1;
    const auto &mc = sys.hw.memcards[slot].data;
    for (int blk = 1; blk <= 15; ++blk) {
        const std::uint8_t *sec = mc.data() + blk * 128;
        if (sec[0] != 0x51u) continue;  // 0x51 = first block of allocated file
        char name_buf[21] = {};
        std::memcpy(name_buf, sec + 10, 20);
        if (fname == name_buf) return blk;
    }
    return -1;
}

bool Bios::fill_direntry(System &sys, int slot, int block, std::uint32_t direntry_addr) const {
    const auto &mc = sys.hw.memcards[slot].data;
    const std::uint8_t *sec = mc.data() + block * 128;
    // struct DIRENTRY { char name[20]; int32_t attr; int32_t size; void *next; int32_t head; char system[4]; }
    for (int i = 0; i < 20; ++i) sys.mem.write8(sys, direntry_addr + static_cast<std::uint32_t>(i), sec[10 + i]);
    std::uint32_t fsize = 0;
    std::memcpy(&fsize, sec + 4, 4);
    sys.mem.write32(sys, direntry_addr + 20u, 0u);  // attr
    sys.mem.write32(sys, direntry_addr + 24u, fsize);
    sys.mem.write32(sys, direntry_addr + 28u, 0u);  // next
    sys.mem.write32(sys, direntry_addr + 32u, static_cast<std::uint32_t>(block));
    sys.mem.write32(sys, direntry_addr + 36u, 0u);
    return true;
}

// ---------------- BIOS Vector Dispatch ----------------

bool Bios::handle_vector(System &sys, std::uint32_t vector_addr) {
    const std::uint32_t masked = vector_addr & 0x1FFFFFFFu;
    const std::uint32_t fn = sys.cpu.r[9] & 0xFFu;  // $t1 holds BIOS function index
    sys.cpu.flush_load_delay();

    if (masked == 0xA0u) {
        call_a0(sys, fn);
        sys.cpu.pc = sys.cpu.r[31];
        sys.cpu.npc = sys.cpu.pc + 4u;
        return true;
    }
    if (masked == 0xB0u) {
        call_b0(sys, fn);
        if (fn != 0x17u) {  // ReturnFromException sets PC itself
            sys.cpu.pc = sys.cpu.r[31];
            sys.cpu.npc = sys.cpu.pc + 4u;
        }
        return true;
    }
    if (masked == 0xC0u) {
        call_c0(sys, fn);
        sys.cpu.pc = sys.cpu.r[31];
        sys.cpu.npc = sys.cpu.pc + 4u;
        return true;
    }
    return false;
}

void Bios::call_a0(System &sys, std::uint32_t fn) {
    auto &r = sys.cpu.r;
    switch (fn) {
    case 0x00u: case 0x02u: case 0x03u: case 0x04u:  // open/read/write/close mirror B0
        call_b0(sys, fn + 0x32u);
        break;

    case 0x17u: {  // strcmp(a0, a1)
        const std::string a = read_guest_cstr(sys, r[4]);
        const std::string b = read_guest_cstr(sys, r[5]);
        r[2] = static_cast<std::uint32_t>(std::strcmp(a.c_str(), b.c_str()));
        break;
    }
    case 0x18u: {  // strncmp(a0, a1, a2)
        const std::string a = read_guest_cstr(sys, r[4], r[6]);
        const std::string b = read_guest_cstr(sys, r[5], r[6]);
        r[2] = static_cast<std::uint32_t>(std::strncmp(a.c_str(), b.c_str(), r[6]));
        break;
    }
    case 0x19u: {  // strcpy(dst, src)
        const std::string s = read_guest_cstr(sys, r[5], 4096);
        for (std::size_t i = 0; i <= s.size(); ++i) {
            sys.mem.write8(sys, r[4] + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(s[i]));
        }
        r[2] = r[4];
        break;
    }
    case 0x1Bu: {  // strlen(a0)
        r[2] = static_cast<std::uint32_t>(read_guest_cstr(sys, r[4], 65536).size());
        break;
    }
    case 0x2Au: {  // memcpy(dst, src, len)
        for (std::uint32_t i = 0; i < r[6]; ++i) {
            sys.mem.write8(sys, r[4] + i, sys.mem.read8(sys, r[5] + i));
        }
        r[2] = r[4];
        break;
    }
    case 0x2Bu: {  // memset(dst, ch, len)
        const std::uint8_t ch = static_cast<std::uint8_t>(r[5] & 0xFFu);
        for (std::uint32_t i = 0; i < r[6]; ++i) {
            sys.mem.write8(sys, r[4] + i, ch);
        }
        r[2] = r[4];
        break;
    }
    case 0x2Cu: {  // memmove(dst, src, len)
        std::vector<std::uint8_t> tmp(r[6]);
        for (std::uint32_t i = 0; i < r[6]; ++i) tmp[i] = sys.mem.read8(sys, r[5] + i);
        for (std::uint32_t i = 0; i < r[6]; ++i) sys.mem.write8(sys, r[4] + i, tmp[i]);
        r[2] = r[4];
        break;
    }
    case 0x2Du: {  // memcmp(a, b, len)
        int diff = 0;
        for (std::uint32_t i = 0; i < r[6]; ++i) {
            const int ca = sys.mem.read8(sys, r[4] + i);
            const int cb = sys.mem.read8(sys, r[5] + i);
            if (ca != cb) { diff = ca - cb; break; }
        }
        r[2] = static_cast<std::uint32_t>(diff);
        break;
    }
    case 0x33u: {  // malloc(size)
        const std::uint32_t sz = (r[4] + 3u) & ~3u;
        if (heap_cur_ + sz <= heap_end_) {
            r[2] = heap_cur_;
            heap_cur_ += sz;
        } else {
            r[2] = 0u;
        }
        break;
    }
    case 0x34u:  // free
        break;
    case 0x39u:  // InitHeap(addr, size)
        heap_base_ = r[4];
        heap_cur_ = r[4];
        heap_end_ = r[4] + r[5];
        break;
    case 0x3Au:  // _96_init (ISO9660 CD filesystem init)
        r[2] = 1u;
        break;
    case 0x3Cu:  // putchar(ch)
        if (r[4] == '\n') {
            std::fprintf(stderr, "[psx-stdout] %s\n", stdout_buf_.c_str());
            stdout_buf_.clear();
        } else if (r[4] >= 0x20u && r[4] < 0x7Fu) {
            stdout_buf_.push_back(static_cast<char>(r[4]));
        }
        r[2] = r[4];
        break;
    case 0x3Eu: {  // puts(s)
        const std::string s = read_guest_cstr(sys, r[4], 512);
        std::fprintf(stderr, "[psx-stdout] %s\n", s.c_str());
        r[2] = 0u;
        break;
    }
    case 0x3Fu: {  // printf(fmt, ...)
        const std::string fmt = read_guest_cstr(sys, r[4], 512);
        std::fprintf(stderr, "[psx-printf] %s", fmt.c_str());
        if (fmt.empty() || fmt.back() != '\n') std::fprintf(stderr, "\n");
        r[2] = static_cast<std::uint32_t>(fmt.size());
        break;
    }
    case 0x44u:  // FlushCache
        sys.mem.global_code_gen++;
        for (auto &g : sys.mem.page_gen) ++g;
        break;
    case 0x49u:  // GPU_cw(cmd)
        if (sys.gpu) sys.gpu->write_gp0(r[4]);
        break;
    case 0x51u:  // LoadExec (stubbed cleanly)
        r[2] = 0u;
        break;
    case 0x70u:  // _bu_init (Memory card BIOS filesystem init)
        card_started_ = true;
        r[2] = 1u;
        break;
    case 0x71u:  // _96_remove
        r[2] = 0u;
        break;
    case 0x72u:  // SetMem(megabytes) — PS1 2MB RAM
        sys.hw.ram_size_reg = (r[4] == 8u) ? 0x00000B98u : 0x00000B88u;
        break;
    case 0x9Fu:  // SetMemSize
        break;
    case 0xABu: {  // _card_info(chan)
        const int slot = (r[4] >> 4) & 1;
        const bool present = sys.hw.memcards[slot].present;
        card_last_status_[slot] = present ? 0u : 2u;
        deliver_event(sys, kEvClassSwCard, present ? kEvSpecIoEnd : 0x8000u);
        deliver_event(sys, kEvClassHwCard, present ? kEvSpecIoEnd : 0x8000u);
        r[2] = 1u;
        break;
    }
    case 0xACu: {  // _card_load(chan)
        const int slot = (r[4] >> 4) & 1;
        const bool present = sys.hw.memcards[slot].present;
        deliver_event(sys, kEvClassSwCard, present ? kEvSpecIoEnd : 0x8000u);
        deliver_event(sys, kEvClassHwCard, present ? kEvSpecIoEnd : 0x8000u);
        r[2] = 1u;
        break;
    }
    default:
        r[2] = 0u;
        break;
    }
}

void Bios::call_b0(System &sys, std::uint32_t fn) {
    auto &r = sys.cpu.r;
    switch (fn) {
    case 0x02u: {  // SetRCnt(spec, target, mode)
        const int idx = static_cast<int>(r[4] & 3u);
        if (idx < 3) {
            const std::uint32_t target = r[5] & 0xFFFFu;
            sys.hw.timers[idx].target = target;
            std::uint32_t m = 0x0000u;
            if (target != 0) m |= 0x0008u;      // Reset counter on target
            if (r[6] & 0x1000u) {               // RCntMdINTR
                m |= (target != 0) ? 0x0010u : 0x0020u;  // IRQ on target (or overflow if target==0)
                if ((r[6] & 0x2000u) == 0) {
                    m |= 0x0040u;               // Repeat IRQ (default unless single-shot)
                }
            }
            if (r[6] & 0x0001u) {               // RCntMdSC: clock source (CNT2 uses bit 9 = sysclk/8)
                m |= (idx == 2) ? 0x0200u : 0x0100u;
            }
            sys.hw.timers[idx].mode = m | (1u << 10);
            sys.hw.timers[idx].counter = 0;
            sys.hw.timers[idx].sub_cycles = 0;
            sys.hw.timers[idx].irq_fired = false;
        }
        r[2] = 1u;
        break;
    }
    case 0x03u: {  // GetRCnt(spec)
        const int idx = static_cast<int>(r[4] & 3u);
        r[2] = (idx < 3) ? (sys.hw.timers[idx].counter & 0xFFFFu) : 0u;
        break;
    }
    case 0x04u: {  // StartRCnt(spec)
        const int idx = static_cast<int>(r[4] & 3u);
        if (idx < 3) sys.hw.i_mask |= (1u << (kIrqTimer0 + idx));
        else sys.hw.i_mask |= (1u << kIrqVBlank);
        sys.hw.update_cpu_irq(sys);
        r[2] = 1u;
        break;
    }
    case 0x05u: {  // StopRCnt(spec)
        const int idx = static_cast<int>(r[4] & 3u);
        if (idx < 3) sys.hw.i_mask &= ~(1u << (kIrqTimer0 + idx));
        else sys.hw.i_mask &= ~(1u << kIrqVBlank);
        sys.hw.update_cpu_irq(sys);
        r[2] = 1u;
        break;
    }
    case 0x06u: {  // ResetRCnt(spec)
        const int idx = static_cast<int>(r[4] & 3u);
        if (idx < 3) sys.hw.timers[idx].counter = 0;
        r[2] = 1u;
        break;
    }
    case 0x07u:  // DeliverEvent(class, spec)
        deliver_event(sys, r[4], r[5]);
        break;
    case 0x08u:  // OpenEvent(class, spec, mode, func)
        r[2] = open_event(r[4], r[5], r[6], r[7]);
        break;
    case 0x09u:  // CloseEvent(handle)
        r[2] = close_event(r[4]) ? 1u : 0u;
        break;
    case 0x0Au:  // WaitEvent(handle)
        r[2] = wait_event(sys, r[4]) ? 1u : 0u;
        break;
    case 0x0Bu:  // TestEvent(handle)
        r[2] = test_event(r[4]) ? 1u : 0u;
        break;
    case 0x0Cu:  // EnableEvent(handle)
        r[2] = enable_event(r[4]) ? 1u : 0u;
        break;
    case 0x0Du:  // DisableEvent(handle)
        r[2] = disable_event(r[4]) ? 1u : 0u;
        break;
    case 0x0Eu:  // OpenTh(reg_pc, reg_sp_fp, reg_gp)
        r[2] = 0xFF000001u;
        break;
    case 0x0Fu:  // CloseTh
    case 0x10u:  // ChangeTh
        r[2] = 1u;
        break;
    case 0x12u:  // InitPAD(buf1, len1, buf2, len2)
        pad_buf_[0] = r[4]; pad_len_[0] = r[5];
        pad_buf_[1] = r[6]; pad_len_[1] = r[7];
        r[2] = 1u;
        break;
    case 0x13u:  // StartPAD()
        pad_started_ = true;
        on_vblank(sys);
        r[2] = 1u;
        break;
    case 0x14u:  // StopPAD()
        pad_started_ = false;
        break;
    case 0x15u:  // PAD_init2(type, button_dest, unused, unused)
        pad_buf_[0] = r[5]; pad_len_[0] = 34u;
        pad_started_ = true;
        on_vblank(sys);
        r[2] = 1u;
        break;
    case 0x16u:  // PAD_dr()
        r[2] = static_cast<std::uint32_t>(sys.hw.pads[0].buttons) |
               (static_cast<std::uint32_t>(sys.hw.pads[1].buttons) << 16);
        break;
    case 0x17u:  // ReturnFromException()
        if (in_irq_handler_) {
            sys.cpu.pc = 0xBFC7FFF0u;
            sys.cpu.npc = 0xBFC7FFF4u;
            sys.cpu.budget = 0;  // Immediately unwind nested AOT / interpreter calls back to handle_irq_exception
        } else {
            sys.cpu.pc = sys.cpu.cop0[kCop0Epc];
            sys.cpu.npc = sys.cpu.pc + 4u;
            const std::uint32_t sr = sys.cpu.cop0[kCop0Sr];
            sys.cpu.cop0[kCop0Sr] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
        }
        break;
    case 0x18u:  // SetDefaultExitFromException()
        custom_exit_buf_ = 0;
        break;
    case 0x19u:  // SetCustomExitFromException(addr)
        custom_exit_buf_ = r[4];
        break;
    case 0x20u:  // UnDeliverEvent(class, spec)
        undeliver_event(r[4], r[5]);
        break;

    // ---- Memory Card bu00:/bu10: File I/O ----
    case 0x32u: {  // open(filename, accessmode)
        const std::string path = read_guest_cstr(sys, r[4], 128);
        std::string fname;
        const int slot = parse_card_path(path, fname);
        if (slot < 0 || !sys.hw.memcards[slot].present) {
            r[2] = 0xFFFFFFFFu;
            break;
        }
        const bool create = (r[5] & 0x0200u) != 0;
        const bool write_mode = (r[5] & 0x0002u) != 0 || create;
        int blk = find_card_file(sys, slot, fname);

        if (blk < 0 && create) {
            const std::uint32_t blocks_needed = std::max(1u, (r[5] >> 16) & 0xFFFFu);
            auto &mc = sys.hw.memcards[slot];
            // Find `blocks_needed` free blocks in 1..15
            std::vector<int> free_blks;
            for (int b = 1; b <= 15 && free_blks.size() < blocks_needed; ++b) {
                if (mc.data[b * 128] == 0xA0u) free_blks.push_back(b);
            }
            if (free_blks.size() < blocks_needed) {
                r[2] = 0xFFFFFFFFu;
                break;
            }
            blk = free_blks.front();
            const std::uint32_t fsize = blocks_needed * 8192u;
            for (std::size_t i = 0; i < free_blks.size(); ++i) {
                const int b = free_blks[i];
                std::uint8_t *sec = mc.data.data() + b * 128;
                std::memset(sec, 0, 128);
                sec[0] = (i == 0) ? 0x51u : ((i + 1 == free_blks.size()) ? 0x53u : 0x52u);
                if (i == 0) {
                    std::memcpy(sec + 4, &fsize, 4);
                    std::strncpy(reinterpret_cast<char *>(sec + 10), fname.c_str(), 20);
                }
                const std::uint16_t nxt = (i + 1 < free_blks.size()) ? static_cast<std::uint16_t>(free_blks[i + 1] - 1) : 0xFFFFu;
                std::memcpy(sec + 8, &nxt, 2);
                std::uint8_t cs = 0;
                for (int k = 0; k < 127; ++k) cs ^= sec[k];
                sec[127] = cs;
            }
            mc.flag_fresh = false;
            mc.dirty = true;
            mc.save();
        }

        if (blk < 0) {
            r[2] = 0xFFFFFFFFu;
            break;
        }

        int fd = -1;
        for (int i = 2; i < static_cast<int>(kMaxFds); ++i) {
            if (!fds_[i].open) { fd = i; break; }
        }
        if (fd < 0) { r[2] = 0xFFFFFFFFu; break; }
        const std::uint8_t *sec = sys.hw.memcards[slot].data.data() + blk * 128;
        std::uint32_t fsize = 0;
        std::memcpy(&fsize, sec + 4, 4);
        fds_[fd].open = true;
        fds_[fd].slot = slot;
        fds_[fd].filename = fname;
        fds_[fd].start_block = blk;
        fds_[fd].size = fsize;
        fds_[fd].pos = 0;
        fds_[fd].write_mode = write_mode;
        r[2] = static_cast<std::uint32_t>(fd);
        break;
    }
    case 0x33u: {  // lseek(fd, offset, whence)
        const std::uint32_t fd = r[4];
        if (fd < kMaxFds && fds_[fd].open) {
            const std::int32_t off = static_cast<std::int32_t>(r[5]);
            if (r[6] == 0u) fds_[fd].pos = static_cast<std::uint32_t>(std::max<std::int32_t>(0, off));
            else if (r[6] == 1u) fds_[fd].pos = static_cast<std::uint32_t>(std::max<std::int32_t>(0, static_cast<std::int32_t>(fds_[fd].pos) + off));
            else if (r[6] == 2u) fds_[fd].pos = static_cast<std::uint32_t>(std::max<std::int32_t>(0, static_cast<std::int32_t>(fds_[fd].size) + off));
            r[2] = fds_[fd].pos;
        } else {
            r[2] = 0xFFFFFFFFu;
        }
        break;
    }
    case 0x34u: {  // read(fd, dst, len)
        const std::uint32_t fd = r[4];
        if (fd >= kMaxFds || !fds_[fd].open) { r[2] = 0xFFFFFFFFu; break; }
        auto &fh = fds_[fd];
        const auto &mc = sys.hw.memcards[fh.slot].data;
        const std::uint32_t avail = (fh.pos < fh.size) ? (fh.size - fh.pos) : 0u;
        const std::uint32_t to_read = std::min(r[6], avail);
        // Walk linked blocks
        std::uint32_t copied = 0;
        while (copied < to_read) {
            std::uint32_t blk_idx = (fh.pos + copied) / 8192u;
            const std::uint32_t blk_off = (fh.pos + copied) % 8192u;
            int cur_blk = fh.start_block;
            while (blk_idx > 0 && cur_blk >= 1 && cur_blk <= 15) {
                std::uint16_t nxt = 0xFFFFu;
                std::memcpy(&nxt, mc.data() + cur_blk * 128 + 8, 2);
                if (nxt == 0xFFFFu) { cur_blk = -1; break; }
                cur_blk = static_cast<int>(nxt) + 1;
                --blk_idx;
            }
            if (cur_blk < 1 || cur_blk > 15) break;
            const std::uint32_t chunk = std::min(to_read - copied, 8192u - blk_off);
            const std::uint8_t *src = mc.data() + cur_blk * 8192u + blk_off;
            for (std::uint32_t i = 0; i < chunk; ++i) {
                sys.mem.write8(sys, r[5] + copied + i, src[i]);
            }
            copied += chunk;
        }
        fh.pos += copied;
        r[2] = copied;
        break;
    }
    case 0x35u: {  // write(fd, src, len)
        const std::uint32_t fd = r[4];
        if (fd == 1u || fd == 2u) {  // stdout / stderr
            const std::string msg = read_guest_cstr(sys, r[5], std::min<std::uint32_t>(r[6], 512u));
            std::fprintf(stderr, "[psx-fd%u] %s\n", fd, msg.c_str());
            r[2] = r[6];
            break;
        }
        if (fd >= kMaxFds || !fds_[fd].open) { r[2] = 0xFFFFFFFFu; break; }
        auto &fh = fds_[fd];
        auto &mc = sys.hw.memcards[fh.slot];
        const std::uint32_t avail = (fh.pos < fh.size) ? (fh.size - fh.pos) : 0u;
        const std::uint32_t to_write = std::min(r[6], avail);
        std::uint32_t written = 0;
        while (written < to_write) {
            std::uint32_t blk_idx = (fh.pos + written) / 8192u;
            const std::uint32_t blk_off = (fh.pos + written) % 8192u;
            int cur_blk = fh.start_block;
            while (blk_idx > 0 && cur_blk >= 1 && cur_blk <= 15) {
                std::uint16_t nxt = 0xFFFFu;
                std::memcpy(&nxt, mc.data.data() + cur_blk * 128 + 8, 2);
                if (nxt == 0xFFFFu) { cur_blk = -1; break; }
                cur_blk = static_cast<int>(nxt) + 1;
                --blk_idx;
            }
            if (cur_blk < 1 || cur_blk > 15) break;
            const std::uint32_t chunk = std::min(to_write - written, 8192u - blk_off);
            std::uint8_t *dst = mc.data.data() + cur_blk * 8192u + blk_off;
            for (std::uint32_t i = 0; i < chunk; ++i) {
                dst[i] = sys.mem.read8(sys, r[5] + written + i);
            }
            written += chunk;
        }
        fh.pos += written;
        mc.flag_fresh = false;
        mc.dirty = true;
        mc.save();
        r[2] = written;
        break;
    }
    case 0x36u: {  // close(fd)
        const std::uint32_t fd = r[4];
        if (fd < kMaxFds && fds_[fd].open) {
            fds_[fd] = FileHandle{};
            r[2] = fd;
        } else {
            r[2] = 0xFFFFFFFFu;
        }
        break;
    }
    case 0x38u:  // ioctl
        r[2] = 0u;
        break;
    case 0x41u: {  // format(devname)
        const std::string path = read_guest_cstr(sys, r[4], 64);
        std::string dummy;
        const int slot = parse_card_path(path, dummy);
        if (slot >= 0 && sys.hw.memcards[slot].present) {
            sys.hw.memcards[slot].format_empty();
            sys.hw.memcards[slot].save();
            r[2] = 1u;
        } else {
            r[2] = 0u;
        }
        break;
    }
    case 0x42u: {  // firstfile(pattern, direntry)
        const std::string path = read_guest_cstr(sys, r[4], 128);
        std::string pat;
        const int slot = parse_card_path(path, pat);
        if (slot < 0 || !sys.hw.memcards[slot].present) { r[2] = 0u; break; }
        dir_search_.slot = slot;
        dir_search_.pattern = pat.empty() ? "*" : pat;
        dir_search_.next_block = 1;
        // Fall into nextfile logic
        r[2] = 0u;
        const auto &mc = sys.hw.memcards[slot].data;
        while (dir_search_.next_block <= 15) {
            const int b = dir_search_.next_block++;
            const std::uint8_t *sec = mc.data() + b * 128;
            if (sec[0] != 0x51u) continue;
            char name_buf[21] = {};
            std::memcpy(name_buf, sec + 10, 20);
            if (wildcard_match(dir_search_.pattern, name_buf)) {
                fill_direntry(sys, slot, b, r[5]);
                r[2] = r[5];
                break;
            }
        }
        break;
    }
    case 0x43u: {  // nextfile(direntry)
        const int slot = dir_search_.slot;
        r[2] = 0u;
        if (slot >= 0 && slot <= 1 && sys.hw.memcards[slot].present) {
            const auto &mc = sys.hw.memcards[slot].data;
            while (dir_search_.next_block <= 15) {
                const int b = dir_search_.next_block++;
                const std::uint8_t *sec = mc.data() + b * 128;
                if (sec[0] != 0x51u) continue;
                char name_buf[21] = {};
                std::memcpy(name_buf, sec + 10, 20);
                if (wildcard_match(dir_search_.pattern, name_buf)) {
                    fill_direntry(sys, slot, b, r[4]);
                    r[2] = r[4];
                    break;
                }
            }
        }
        break;
    }
    case 0x45u: {  // erase(filename)
        const std::string path = read_guest_cstr(sys, r[4], 128);
        std::string fname;
        const int slot = parse_card_path(path, fname);
        const int start = (slot >= 0) ? find_card_file(sys, slot, fname) : -1;
        if (start < 0) { r[2] = 0u; break; }
        auto &mc = sys.hw.memcards[slot];
        int cur = start;
        for (int guard = 0; cur >= 1 && cur <= 15 && guard < 16; ++guard) {
            std::uint8_t *sec = mc.data.data() + cur * 128;
            std::uint16_t nxt = 0xFFFFu;
            std::memcpy(&nxt, sec + 8, 2);
            sec[0] = 0xA0u;
            sec[8] = 0xFFu;
            sec[9] = 0xFFu;
            std::uint8_t cs = 0;
            for (int k = 0; k < 127; ++k) cs ^= sec[k];
            sec[127] = cs;
            cur = (nxt == 0xFFFFu) ? -1 : (static_cast<int>(nxt) + 1);
        }
        mc.dirty = true;
        mc.save();
        r[2] = 1u;
        break;
    }
    case 0x4Au:  // InitCARD(pad_enable)
        card_shared_irq_ = (r[4] != 0);
        break;
    case 0x4Bu:  // StartCARD()
        card_started_ = true;
        break;
    case 0x4Cu:  // StopCARD()
        card_started_ = false;
        break;
    case 0x4Eu: {  // _card_write(chan, block, dst)
        const int slot = (r[4] >> 4) & 1;
        const std::uint32_t sector = r[5];
        if (sys.hw.memcards[slot].present && sector < 1024u) {
            auto &mc = sys.hw.memcards[slot];
            for (std::uint32_t i = 0; i < 128u; ++i) {
                mc.data[sector * 128u + i] = sys.mem.read8(sys, r[6] + i);
            }
            mc.flag_fresh = false;
            mc.dirty = true;
            mc.save();
            deliver_event(sys, kEvClassHwCard, kEvSpecIoEnd);
            deliver_event(sys, kEvClassSwCard, kEvSpecIoEnd);
            r[2] = 1u;
        } else {
            deliver_event(sys, kEvClassHwCard, 0x8000u);
            r[2] = 0u;
        }
        break;
    }
    case 0x4Fu: {  // _card_read(chan, block, dst)
        const int slot = (r[4] >> 4) & 1;
        const std::uint32_t sector = r[5];
        if (sys.hw.memcards[slot].present && sector < 1024u) {
            const auto &mc = sys.hw.memcards[slot];
            for (std::uint32_t i = 0; i < 128u; ++i) {
                sys.mem.write8(sys, r[6] + i, mc.data[sector * 128u + i]);
            }
            deliver_event(sys, kEvClassHwCard, kEvSpecIoEnd);
            deliver_event(sys, kEvClassSwCard, kEvSpecIoEnd);
            r[2] = 1u;
        } else {
            deliver_event(sys, kEvClassHwCard, 0x8000u);
            r[2] = 0u;
        }
        break;
    }
    case 0x50u:  // _new_card()
        deliver_event(sys, kEvClassSwCard, kEvSpecNew);
        break;
    case 0x5Bu:  // ChangeClearPAD(val)
        clear_pad_ = (r[4] != 0);
        break;
    case 0x5Cu: {  // _card_status(slot)
        const int slot = (r[4] >> 4) & 1;
        r[2] = card_last_status_[slot];
        break;
    }
    case 0x3Du:  // putchar
        call_a0(sys, 0x3Cu);
        break;
    case 0x3Fu:  // puts
        call_a0(sys, 0x3Eu);
        break;
    case 0x56u:  // GetC0Table
        r[2] = 0x80000674u;
        break;
    case 0x57u:  // GetB0Table
        r[2] = 0x80000874u;
        break;
    default:
        r[2] = 0u;
        break;
    }
}

void Bios::call_c0(System &sys, std::uint32_t fn) {
    auto &r = sys.cpu.r;
    switch (fn) {
    case 0x02u:  // SysEnqIntRP(priority, struc)
        sys_enq_int_rp(sys, r[4], r[5]);
        r[2] = 0u;
        break;
    case 0x03u:  // SysDeqIntRP(priority, struc)
        sys_deq_int_rp(sys, r[4], r[5]);
        r[2] = 0u;
        break;
    case 0x0Au: {  // ChangeClearRCnt(t, flag)
        const int idx = static_cast<int>(r[4] & 3u);
        const bool old = clear_rcnt_[idx];
        clear_rcnt_[idx] = (r[5] != 0);
        r[2] = old ? 1u : 0u;
        break;
    }
    default:
        r[2] = 0u;
        break;
    }
}

void Bios::serialize_state(std::ostream &os) const {
    auto wr = [&](const void *p, std::size_t n) {
        os.write(reinterpret_cast<const char *>(p), static_cast<std::streamsize>(n));
    };
    wr(events_.data(), events_.size() * sizeof(Event));
    wr(intr_heads_, sizeof(intr_heads_));
    wr(&custom_exit_buf_, sizeof(custom_exit_buf_));
    wr(pad_buf_, sizeof(pad_buf_));
    wr(pad_len_, sizeof(pad_len_));
    wr(&pad_started_, sizeof(pad_started_));
    wr(&clear_pad_, sizeof(clear_pad_));
    wr(clear_rcnt_, sizeof(clear_rcnt_));
    wr(&card_started_, sizeof(card_started_));
    wr(&card_shared_irq_, sizeof(card_shared_irq_));
    wr(card_last_status_, sizeof(card_last_status_));
    wr(&heap_base_, sizeof(heap_base_));
    wr(&heap_cur_, sizeof(heap_cur_));
    wr(&heap_end_, sizeof(heap_end_));
    wr(&current_thread_, sizeof(current_thread_));
}

bool Bios::deserialize_state(std::istream &is) {
    auto rd = [&](void *p, std::size_t n) -> bool {
        is.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(n));
        return is.gcount() == static_cast<std::streamsize>(n);
    };
    if (!rd(events_.data(), events_.size() * sizeof(Event))) return false;
    if (!rd(intr_heads_, sizeof(intr_heads_))) return false;
    if (!rd(&custom_exit_buf_, sizeof(custom_exit_buf_))) return false;
    if (!rd(pad_buf_, sizeof(pad_buf_))) return false;
    if (!rd(pad_len_, sizeof(pad_len_))) return false;
    if (!rd(&pad_started_, sizeof(pad_started_))) return false;
    if (!rd(&clear_pad_, sizeof(clear_pad_))) return false;
    if (!rd(clear_rcnt_, sizeof(clear_rcnt_))) return false;
    if (!rd(&card_started_, sizeof(card_started_))) return false;
    if (!rd(&card_shared_irq_, sizeof(card_shared_irq_))) return false;
    if (!rd(card_last_status_, sizeof(card_last_status_))) return false;
    if (!rd(&heap_base_, sizeof(heap_base_))) return false;
    if (!rd(&heap_cur_, sizeof(heap_cur_))) return false;
    if (!rd(&heap_end_, sizeof(heap_end_))) return false;
    if (!rd(&current_thread_, sizeof(current_thread_))) return false;
    in_irq_handler_ = false;
    fds_.fill(FileHandle{});
    return true;
}

}  // namespace psx
