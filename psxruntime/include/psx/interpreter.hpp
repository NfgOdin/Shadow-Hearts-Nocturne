#pragma once

#include <cstdint>

namespace psx {

class System;

// Execute a single R3000A instruction at sys.cpu.pc (including accurate load-delay & branch-delay).
void step_instruction(System &sys);

// Execute instructions in interpreter mode until budget expires, halt, or return to a target PC.
// If stop_pc != 0, stops immediately when sys.cpu.pc == stop_pc (used for BIOS guest callbacks).
void run_interpreter(System &sys, std::uint32_t stop_pc = 0);

// Primary entry point for AOT-compiled code to call/jump to any guest address:
// Dispatches to verified AOT function if present and RAM hash matches, handles BIOS vectors
// (0xA0/0xB0/0xC0), or seamlessly falls back to the R3000A+GTE interpreter.
void dispatch(System &sys, std::uint32_t target_pc);

// Call a guest subroutine and return when it returns to the synthetic sentinel address.
void call_guest_function(System &sys, std::uint32_t func_pc, std::uint32_t max_cycles = 2000000u);

}  // namespace psx
