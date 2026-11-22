#pragma once

#include <functional>
#include <string>

namespace coop { struct LaunchConfig; }

bool open_window(bool fullscreen);
int run_window(const std::function<int()> &emulate);
void configure_coop(const coop::LaunchConfig &cfg, const std::string &game_root_dir);
// reads SDL's events, the keyboard and the game controllers into the console's two controller ports (once a frame in a window
// run); host/pad_test.cpp calls it without a window
void poll_input();
