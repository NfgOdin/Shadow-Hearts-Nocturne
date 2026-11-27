#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace psx {

class System;

// PlayStation 1 Kernel & BIOS HLE (A0 / B0 / C0 vector tables, Events, Interrupt Chains,
// Threads, Pad, Memory Card bu00:/bu10:, and SYSCALL handler).
class Bios {
public:
    static constexpr std::uint32_t kMaxEvents = 64;
    static constexpr std::uint32_t kMaxThreads = 16;
    static constexpr std::uint32_t kMaxFds = 16;

    // Guest-RAM layout for kernel control blocks (matches standard PS1 BIOS low-RAM map)
    static constexpr std::uint32_t kTcbTableAddr = 0x80000108u;       // ptr to TCB array (PsyQ reads [0x80000108])
    static constexpr std::uint32_t kTcbArrayAddr = 0x80001000u;       // 16 x 0xC0 byte TCBs
    static constexpr std::uint32_t kIntRpTableAddr = 0x80000200u;     // 4 priority head pointers
    static constexpr std::uint32_t kDefaultExitBufAddr = 0x80000160u;

    struct Event {
        bool used = false;
        std::uint32_t class_id = 0;
        std::uint32_t spec = 0;
        std::uint32_t mode = 0;       // 0x1000 = callback, 0x2000 = no-callback (poll/wait)
        std::uint32_t func = 0;       // callback PC
        std::uint32_t status = 0;     // 0x0000=free, 0x1000=disabled, 0x2000=enabled/busy, 0x4000=ready
    };

    struct FileHandle {
        bool open = false;
        int slot = 0;                 // 0 = bu00:, 1 = bu10:
        std::string filename;
        int start_block = -1;         // 1..15 in 128KB memory card
        std::uint32_t block_count = 0;
        std::uint32_t size = 0;
        std::uint32_t pos = 0;
        bool write_mode = false;
    };

    struct DirSearch {
        int slot = 0;
        std::string pattern;
        int next_block = 1;
    };

    void reset(System &sys);

    // Populate low RAM and 0xBFC00000 BIOS ROM with clean HLE trampolines & version header.
    void install_bios_stubs(System &sys);

    // Dispatch BIOS vector calls (PC == 0x000000A0, 0x000000B0, 0x000000C0, masked).
    // Returns true if handled cleanly; sets $v0 and $pc = $ra (unless ReturnFromException/ChangeTh).
    bool handle_vector(System &sys, std::uint32_t vector_addr);

    // Handle R3000A SYSCALL (A0=1 EnterCriticalSection, A0=2 ExitCriticalSection, A0=3 ChangeThreadSubFunction).
    void handle_syscall(System &sys);

    // Handle hardware IRQ exception (saves context, runs SysEnqIntRP chains, delivers BIOS events, restores).
    void handle_irq_exception(System &sys);

    // Called once per VBlank to refresh BIOS PAD buffers and deliver VBlank events if needed.
    void on_vblank(System &sys);

    // Deliver a BIOS event by (class_id, spec).
    void deliver_event(System &sys, std::uint32_t class_id, std::uint32_t spec);

    bool change_clear_rcnt(int idx) const noexcept { return (idx >= 0 && idx < 4) ? clear_rcnt_[idx] : true; }
    bool change_clear_pad() const noexcept { return clear_pad_; }

    void serialize_state(std::ostream &os) const;
    bool deserialize_state(std::istream &is);
    void clear_pending_card_event() const noexcept;

private:
    void call_a0(System &sys, std::uint32_t fn);
    void call_b0(System &sys, std::uint32_t fn);
    void call_c0(System &sys, std::uint32_t fn);

    // Event management
    std::uint32_t open_event(std::uint32_t class_id, std::uint32_t spec, std::uint32_t mode, std::uint32_t func);
    bool close_event(std::uint32_t handle);
    bool enable_event(std::uint32_t handle);
    bool disable_event(std::uint32_t handle);
    bool test_event(std::uint32_t handle);
    bool wait_event(System &sys, std::uint32_t handle);
    void undeliver_event(std::uint32_t class_id, std::uint32_t spec);

    // Interrupt priority chain (SysEnqIntRP / SysDeqIntRP)
    void sys_enq_int_rp(System &sys, std::uint32_t priority, std::uint32_t node_addr);
    void sys_deq_int_rp(System &sys, std::uint32_t priority, std::uint32_t node_addr);
    void invoke_guest_callback(System &sys, std::uint32_t func_pc, std::uint32_t a0 = 0, std::uint32_t a1 = 0);

    // Memory card bu00:/bu10: filesystem helpers
    int parse_card_path(const std::string &path, std::string &fname_out) const;
    int find_card_file(const System &sys, int slot, const std::string &fname) const;
    bool fill_direntry(System &sys, int slot, int block, std::uint32_t direntry_addr) const;
    static bool wildcard_match(const std::string &pat, const std::string &str);

    // Guest string reader
    static std::string read_guest_cstr(System &sys, std::uint32_t addr, std::size_t max_len = 256);

    std::array<Event, kMaxEvents> events_ = {};
    std::array<FileHandle, kMaxFds> fds_ = {};
    DirSearch dir_search_ = {};

    // Interrupt priority chain heads (guest addresses, 4 priorities 0..3)
    std::uint32_t intr_heads_[4] = {};

    // Exception exit hook (SetCustomExitFromException)
    std::uint32_t custom_exit_buf_ = 0;

    // BIOS PAD buffers (InitPAD / StartPAD)
    std::uint32_t pad_buf_[2] = {};
    std::uint32_t pad_len_[2] = {};
    bool pad_started_ = false;
    bool clear_pad_ = false;
    bool clear_rcnt_[4] = {true, true, true, true};

    // Card BIOS state
    bool card_started_ = false;
    bool card_shared_irq_ = false;
    std::uint32_t card_last_status_[2] = {};

    // Heap / misc
    std::uint32_t heap_base_ = 0;
    std::uint32_t heap_cur_ = 0;
    std::uint32_t heap_end_ = 0;
    std::uint32_t current_thread_ = 0;
    bool in_irq_handler_ = false;

    // Console output line buffer for BIOS printf/putchar
    std::string stdout_buf_;
};

}  // namespace psx
