#include "window.hpp"
#include "big_stack_thread.hpp"
#include "stick_map.hpp"

#include "ps2/system.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <timeapi.h>
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <SDL_metal.h>
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <csignal>
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "achievements.hpp"
#include "keymap.hpp"
#include "quickmenu.hpp"
#include "screenshot.hpp"
#include "ps2/crash.hpp"
#include "ps2/disc.hpp"
#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "ps2/video.hpp"
#include "stretch.hpp"
#ifdef PS2_GPU
#include "../runtime/src/gs/gpu/overlay.hpp"  // the quick menu's panel in the WebGPU present pass
#endif

namespace {

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

SDL_Window *g_window = nullptr;
#ifdef __APPLE__
SDL_MetalView g_metal_view = nullptr;
#endif
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
std::atomic<SDL_AudioDeviceID> g_audio{0};
ps2sound::SoundPipe *g_pipe = nullptr;  // the game's sound: pushed by the emulation thread, played (and stretched) by the device thread, host/stretch.hpp
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
// PS2_PAD_LOG=1 prints every change of what the two controller ports send to the game, and every rumble request
bool g_pad_log = false;
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::atomic<std::uint64_t> g_last{0};
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;

// Where the window thread is in its own loop ("asking the system for events", "drawing") and since when. Always on: two stores per
// step. The Linux watchdog prints it when no frame comes, so a window thread that is stuck in one call names the call (docs/linux.md,
// "The window freeze").
std::atomic<const char *> g_window_stage{nullptr};
std::atomic<std::int64_t> g_window_stage_since_ns{0};
void window_stage(const char *name) {
    g_window_stage_since_ns.store(ps2::hitch::now_ns(), std::memory_order_relaxed);
    g_window_stage.store(name, std::memory_order_release);
}

// --- keyboard and mouse (host/keymap.cpp, docs/keys.md) ---
//
// Which key works which button comes from keys.ini (written with the defaults on the first start, read again when the window
// gets the focus back after the file changed). The arrow keys are the D-pad only: they used to push the left stick as well, and
// a menu that reads both moved two entries for one press.
keymap::Config g_keys = keymap::defaults();
std::string g_keys_path;
std::filesystem::file_time_type g_keys_time{};
int g_mouse_sum_x = 0, g_mouse_sum_y = 0;  // mouse counts since the last sample (only while the mouse pushes a stick)
Uint32 g_mouse_sample = 0;
double g_mouse_vx = 0.0, g_mouse_vy = 0.0;  // counts per millisecond, smoothed
Uint32 g_wheel_up_until = 0, g_wheel_down_until = 0;
int g_last_dx = 0, g_last_dy = -1;  // the last direction the walk keys gave, for a roll without one

// The picture look (F2): how the picture is enlarged and smoothed for the window (display only, the game and the saved pictures do not
// change). The choice sits in keys.ini ('look = ...'); PS2_LOOK=0..5 or a look word overrides it for a run and is not written.
int g_look = keymap::kDefaultLook;
bool g_look_forced = false;

// The title shows the speed; for a few seconds after F5, F8 or F9 a message is put in front of it.
std::string g_speed_title = "ps2-native-recomp";
std::string g_game_title;  // optional PS2_WINDOW_TITLE, supplied by the launcher
std::string g_notice_text;
const char *const kStartNotice = "F1 = menu";  // the title for the first 30 s
Uint32 g_notice_until = 0;
// A short tone mixed into the game's sound says that a key worked, which the title cannot do in full screen. 0 none, else the kind of
// the message (1 saved, 2 loaded, 3 report written, 4 trouble). PS2_NO_BEEP=1 turns it off.
std::atomic<int> g_beep_request{0};
bool g_beep_on = true;

// The quick menu (F1, the extra menu key of keys.ini (none by default), hold Select then press Start or the Guide button on a controller;
// host/quickmenu.cpp). While it is open the
// emulation thread waits at the end of its frame (on_vblank), so the game stands still and makes no sound, and the window thread keeps
// drawing the last picture with the panel over it. The game's controller ports get nothing while it is open, and after it a port gets
// nothing until its buttons are all let go (the Enter or Cross that chose "Resume" must not reach the game).
quickmenu::Menu g_menu;
std::atomic<bool> g_menu_hold{false};
std::string g_key_note;  // the keys page's message line: what the last change did
bool g_menu_redraw = false;  // show the picture again: the panel changed while the game sends none
quickmenu::Picture g_menu_picture;
SDL_Texture *g_menu_texture = nullptr;  // the panel for the SDL renderer (PS2_SDL_RENDERER=1)
int g_menu_texture_w = 0, g_menu_texture_h = 0;
Uint32 g_menu_opened_ticks = 0;
bool g_block_input[2] = {};
int g_stick_step[2] = {};  // the left stick moving the highlight: -1 up, 1 down, 0 near the middle
// The inner resolution (PS2_SCALE): the one of this run, and the one keys.ini holds for the next start (the menu changes that one).
int g_scale_now = 1, g_scale_next = 1;
bool g_scale_locked = false;  // PS2_SCALE was set before the start: keys.ini does not count
// The filter of upscaled 2D art (PS2_SHARP_2D: 0 soft, 1 sharp, 2 sharpest), the same way: this run's and keys.ini's for the next start.
int g_sharp_now = 1, g_sharp_next = 1;
bool g_sharp_locked = false;
// Graphics chip (PS2_GPU_ADAPTER), graphics API (PS2_GPU_BACKEND), and async GPU submit thread (PS2_GPU_SUBMIT_THREAD)
std::string g_gpu_now = "auto", g_gpu_next = "auto";
bool g_gpu_locked = false;
std::string g_gpu_api_now = "dx12", g_gpu_api_next = "dx12";
bool g_gpu_api_locked = false;
bool g_gpu_submit_on = true;
std::atomic<double> g_emu_cpu_ms{0.0};  // main emulation thread CPU time in ms (for single-core main thread % in the title bar)
// A short note drawn over the picture for a moment after the menu is closed ("Saved", "Loaded"): the menu asked for a quick save or load
bool g_note_wanted = false;  // a save or load was asked by the menu: the next message about it is shown over the picture
std::string g_note_text;  // not empty while the note is up
Uint32 g_note_until = 0;
bool g_overlay_on = false;  // g_menu_picture is laid over the game's picture (the menu, or the note)
std::uint64_t g_script_extra = 0;  // the pad script's clock goes on while the menu is open (the game's frames stand still)

// Live Graphics & FPS controls
int g_aspect_mode = 0;       // 0 = 4:3, 1 = 16:9, 2 = Stretch
int g_brightness = 0;        // -50..50
int g_contrast = 100;        // 50..150
int g_saturation = 100;      // 0..200
int g_gamma = 100;           // 50..200
int g_scanlines = 0;         // 0..100
bool g_deinterlace = true;
std::string g_fps_cap = "60";
bool g_fps_speed_sync = true;
int g_display_hz = 60;
std::atomic<double> g_pace_target_hz{60.0};
std::atomic<bool> g_pace_speed_sync{true};
int g_toast_theme = 0;
std::string g_present_mode = "mailbox";
// Phase 2: Depth-aware lighting & post-processing, GPU tile binning, & Save State Slots (0-9)
int g_ssao = 0;
int g_bloom = 0;
int g_ssgi = 0;
bool g_filmic = false;
int g_vignette = 0;
bool g_gpu_binning_on = true;
int g_save_slot = 0;
bool g_slot_picker_on_quickkey = false;
int g_cached_thumb_slot = -1;
std::vector<std::uint32_t> g_cached_thumb_rgba;
int g_cached_thumb_w = 0, g_cached_thumb_h = 0;

// Live Audio controls
int g_master_vol = 100;
bool g_muted = false;
std::string g_audio_device;  // empty = System Default
bool g_audio_stretch = true;
std::atomic<int> g_chime_vol{100};
int g_bgm_vol = 100;
int g_sfx_vol = 100;

// Achievement tracker & top-right animated toast
std::mutex g_ach_mutex;
achievements::Manager g_achievements;
achievements::ToastPicture g_toast_picture;
SDL_Texture *g_toast_texture = nullptr;
int g_toast_texture_w = 0, g_toast_texture_h = 0;

int query_display_hz() {
    if (!g_window) return 60;
    const int disp = SDL_GetWindowDisplayIndex(g_window);
    SDL_DisplayMode mode{};
    if (disp >= 0 && SDL_GetCurrentDisplayMode(disp, &mode) == 0 && mode.refresh_rate >= 30) return mode.refresh_rate;
    return 60;
}

double resolved_target_fps() {
    if (g_fps_cap == "unlimited") return 0.0;
    if (g_fps_cap == "display") return static_cast<double>(std::max(30, g_display_hz));
    const int n = std::atoi(g_fps_cap.c_str());
    if (n >= 15 && n <= 1000) return static_cast<double>(n);
    return ps2::g_options.pal ? 50.0 : 60.0;
}

std::string format_fps_cap_label() {
    if (g_fps_cap == "unlimited") return "Unlimited";
    if (g_fps_cap == "display") return "Match Display (" + std::to_string(std::max(30, g_display_hz)) + " Hz)";
    const int n = std::atoi(g_fps_cap.c_str());
    if (n >= 15) return std::to_string(n) + " FPS";
    return "60 FPS";
}

void apply_video_params() {
    ps2::VideoParams vp;
    vp.brightness = static_cast<float>(g_brightness) / 100.0f;
    vp.contrast = static_cast<float>(g_contrast) / 100.0f;
    vp.saturation = static_cast<float>(g_saturation) / 100.0f;
    vp.gamma = static_cast<float>(g_gamma) / 100.0f;
    vp.scanlines = static_cast<float>(g_scanlines) / 100.0f;
    vp.aspect_mode = g_aspect_mode;
    vp.deinterlace = g_deinterlace;
    vp.ssao = static_cast<float>(g_ssao) / 100.0f;
    vp.bloom = static_cast<float>(g_bloom) / 100.0f;
    vp.ssgi = static_cast<float>(g_ssgi) / 100.0f;
    vp.filmic = g_filmic;
    vp.vignette = static_cast<float>(g_vignette) / 100.0f;
    ps2::set_video_params(vp);
    const double target_hz = resolved_target_fps();
    g_pace_target_hz.store(target_hz, std::memory_order_relaxed);
    g_pace_speed_sync.store(g_fps_speed_sync, std::memory_order_relaxed);
    ps2::set_fps_sync(target_hz, g_fps_speed_sync);
}

void apply_audio_settings() {
    if (g_pipe) {
        g_pipe->set_volume(static_cast<float>(g_master_vol) / 100.0f);
        g_pipe->set_muted(g_muted);
        g_pipe->set_stretch_enabled(g_audio_stretch);
    }
    ps2::set_audio_channel_gains(static_cast<float>(g_bgm_vol) / 100.0f, static_cast<float>(g_sfx_vol) / 100.0f);
}

bool reopen_audio_device(const std::string &device_name);
double audio_seconds();
void audio_callback(void *, Uint8 *stream, int length);
void queue_audio(const std::int16_t *samples, std::size_t frames);

void refresh_title() {
    std::string title = g_speed_title;
    if (!g_notice_text.empty()) title = g_notice_text + "   |   " + title;
    if (!g_game_title.empty()) title = g_game_title + "   |   " + title;
    SDL_SetWindowTitle(g_window, title.c_str());
}

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

#ifdef __APPLE__
void destroy_metal_view() {
    if (!g_metal_view) return;
    SDL_Metal_DestroyView(g_metal_view);
    g_metal_view = nullptr;
}
#endif

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_COCOA)
    case SDL_SYSWM_COCOA:
        g_metal_view = SDL_Metal_CreateView(g_window);
        if (!g_metal_view) return false;
        target.kind = ps2::WindowTarget::Metal;
        target.surface = SDL_Metal_GetLayer(g_metal_view);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
        target.kind = ps2::WindowTarget::Win32;
        target.display = info.info.win.hinstance;
        target.surface = info.info.win.window;
        break;
#endif
    default: return false;
    }
    const bool attached = ps2::attach_window(target);
#ifdef __APPLE__
    if (!attached) destroy_metal_view();
#endif
    return attached;
}

// A controller's stick, stretched from the pad's round gate to the console's square one (host/stick_map.hpp). An axis the keyboard
// or the mouse already pushed (not 0x80) is left alone; the keyboard's own diagonal is shorter on purpose (push_stick).
void stick(SDL_GameController *pad, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, std::uint8_t &x, std::uint8_t &y) {
    std::uint8_t px, py;
    stick_map::to_bytes(SDL_GameControllerGetAxis(pad, ax), SDL_GameControllerGetAxis(pad, ay), px, py);
    if (x == 0x80) x = px;
    if (y == 0x80) y = py;
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    if (g_pad_log && (strong != last_strong[port] || weak != last_weak[port]))
        std::fprintf(stderr, "[pad] frame %llu port %d rumble: large motor %u, small motor %s\n", static_cast<unsigned long long>(ps2::frames_run()), port + 1,
                     motors >> 8, weak ? "on" : "off");
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

const char *pad_kind(SDL_GameController *pad) {
    switch (SDL_GameControllerGetType(pad)) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_PS3: return "DualShock 3";
    case SDL_CONTROLLER_TYPE_PS4: return "DualShock 4";
    case SDL_CONTROLLER_TYPE_PS5: return "DualSense";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    default: return "generic";
    }
}

const char *or_unknown(const char *name) { return name ? name : "a controller"; }

// a new controller takes the first free port, the second one plugs in player 2
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_pads)
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
    for (int port = 0; port < 2; ++port)
        if (!g_pads[port]) {
            g_pads[port] = SDL_GameControllerOpen(device);
            if (g_pads[port]) std::fprintf(stderr, "[pad] %s (%s) in port %d\n", or_unknown(SDL_GameControllerName(g_pads[port])), pad_kind(g_pads[port]), port + 1);
            else std::fprintf(stderr, "[pad] cannot open controller %d: %s\n", device, SDL_GetError());
            return;
        }
    std::fprintf(stderr, "[pad] %s is not used: both ports have a controller\n", or_unknown(SDL_GameControllerNameForIndex(device)));
}

void close_pad(SDL_JoystickID id) {
    for (int port = 0; port < 2; ++port) {
        SDL_GameController *&pad = g_pads[port];
        if (!pad || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) != id) continue;
        std::fprintf(stderr, "[pad] %s unplugged from port %d\n", or_unknown(SDL_GameControllerName(pad)), port + 1);
        SDL_GameControllerClose(pad);
        pad = nullptr;
    }
}

// --- PS2_PAD_SCRIPT: virtual controllers, to test without hardware ---
//
// PS2_PAD_SCRIPT=<file> plugs in virtual game controllers (SDL's virtual joysticks, which go through the same game controller
// code as a real pad) and plays a sequence on them, one step per line, at a video frame of the game:
//   <frame> plug <n>                          plugs in virtual controller n (0 to 3)
//   <frame> unplug <n>                        pulls it out again
//   <frame> button <n> <name> <frames>        holds a button: a b x y back guide start leftstick rightstick leftshoulder
//                                             rightshoulder dpup dpdown dpleft dpright
//   <frame> axis <n> <name> <value> <frames>  holds a stick or a trigger: leftx lefty rightx righty from -32768 to 32767 (0 is
//                                             the middle, negative is left or up), lefttrigger righttrigger from 0 to 32767
// Lines starting with # are comments. Controller input then counts while the window is in the background too, so a test
// does not depend on which window has the focus.
struct Step {
    enum Kind { kPlug, kUnplug, kButton, kAxis };
    std::uint64_t frame = 0, frames = 0;
    Kind kind = kPlug;
    int pad = 0, control = 0, value = 0;
};

struct Hold {
    std::uint64_t until = 0;
    int pad = 0, control = 0;
    bool axis = false;
};

struct VirtualPad {
    SDL_Joystick *joystick = nullptr;
    Uint16 low = 0, high = 0;
};

std::vector<Step> g_script;
std::size_t g_next_step = 0;
std::vector<Hold> g_holds;
VirtualPad g_virtual[4];

int SDLCALL virtual_rumble(void *user, Uint16 low, Uint16 high) {
    const int n = static_cast<int>(reinterpret_cast<std::intptr_t>(user));
    VirtualPad &v = g_virtual[n];
    if (low != v.low || high != v.high)
        std::fprintf(stderr, "[pad] frame %llu virtual controller %d rumbles: low %u high %u\n", static_cast<unsigned long long>(ps2::frames_run()), n, low, high);
    v.low = low;
    v.high = high;
    return 0;
}

Sint16 raw_axis(int control, int value) {
    // the game controller layer maps a trigger's whole joystick range onto 0..32767, so a trigger at rest is the joystick minimum
    if (control == SDL_CONTROLLER_AXIS_TRIGGERLEFT || control == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return static_cast<Sint16>(std::clamp(value * 2 - 32768, -32768, 32767));
    return static_cast<Sint16>(std::clamp(value, -32768, 32767));
}

void load_pad_script(const std::string &path) {
    std::FILE *f = ps2::os::open(path, "r");
    if (!f) {
        std::fprintf(stderr, "[pad] cannot read %s\n", path.c_str());
        return;
    }
    char line[256];
    int number = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++number;
        char what[32] = {}, name[32] = {};
        unsigned long long frame = 0, frames = 0;
        int pad = 0, value = 0;
        if (line[0] == '#') continue;
        const int fields = std::sscanf(line, "%llu %31s %d %31s", &frame, what, &pad, name);
        if (fields <= 0) continue;
        Step step;
        step.frame = frame;
        step.pad = pad;
        const std::string w = what;
        bool ok = fields >= 3 && pad >= 0 && pad < 4;
        if (ok && (w == "plug" || w == "unplug")) step.kind = w == "plug" ? Step::kPlug : Step::kUnplug;
        else if (ok && w == "button" && std::sscanf(line, "%*u %*s %*d %*s %llu", &frames) == 1) {
            step.kind = Step::kButton;
            step.control = SDL_GameControllerGetButtonFromString(name);
            ok = step.control != SDL_CONTROLLER_BUTTON_INVALID;
        } else if (ok && w == "axis" && std::sscanf(line, "%*u %*s %*d %*s %d %llu", &value, &frames) == 2) {
            step.kind = Step::kAxis;
            step.control = SDL_GameControllerGetAxisFromString(name);
            step.value = value;
            ok = step.control != SDL_CONTROLLER_AXIS_INVALID;
        } else ok = false;
        if (!ok) {
            std::fprintf(stderr, "[pad] %s line %d is not a step: %s", path.c_str(), number, line);
            continue;
        }
        step.frames = frames;
        g_script.push_back(step);
    }
    std::fclose(f);
    std::stable_sort(g_script.begin(), g_script.end(), [](const Step &a, const Step &b) { return a.frame < b.frame; });
    std::fprintf(stderr, "[pad] playing %zu steps from %s on virtual controllers\n", g_script.size(), path.c_str());
}

void plug_virtual(int n, bool on) {
    VirtualPad &v = g_virtual[n];
    if (on == (v.joystick != nullptr)) return;
    if (!on) {
        const SDL_JoystickID id = SDL_JoystickInstanceID(v.joystick);
        SDL_JoystickClose(v.joystick);
        v.joystick = nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) {
                SDL_JoystickDetachVirtual(i);
                break;
            }
        return;
    }
    static const char *const kNames[4] = {"ps2run virtual controller 0", "ps2run virtual controller 1", "ps2run virtual controller 2",
                                          "ps2run virtual controller 3"};
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.name = kNames[n];
    desc.userdata = reinterpret_cast<void *>(static_cast<std::intptr_t>(n));
    desc.Rumble = virtual_rumble;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    v.joystick = device >= 0 ? SDL_JoystickOpen(device) : nullptr;
    if (!v.joystick) {
        std::fprintf(stderr, "[pad] cannot attach virtual controller %d: %s\n", n, SDL_GetError());
        return;
    }
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
}

// runs before SDL reads its events, which apply the values set here (and bring the plug and unplug events)
void play_pad_script() {
    static Uint32 last_ticks = SDL_GetTicks(), menu_ms = 0;
    const Uint32 ticks = SDL_GetTicks();
    if (g_menu.is_open()) {
        menu_ms += ticks - last_ticks;
        g_script_extra += menu_ms / 17;  // about one frame of the script per 1/60 s while the game waits
        menu_ms %= 17;
    }
    last_ticks = ticks;
    const std::uint64_t now = ps2::frames_run() + g_script_extra;
    for (auto h = g_holds.begin(); h != g_holds.end();) {
        if (now < h->until) {
            ++h;
            continue;
        }
        if (SDL_Joystick *j = g_virtual[h->pad].joystick) {
            if (h->axis) SDL_JoystickSetVirtualAxis(j, h->control, raw_axis(h->control, 0));
            else SDL_JoystickSetVirtualButton(j, h->control, SDL_RELEASED);
        }
        h = g_holds.erase(h);
    }
    for (; g_next_step < g_script.size() && g_script[g_next_step].frame <= now; ++g_next_step) {
        const Step &s = g_script[g_next_step];
        if (s.kind == Step::kPlug || s.kind == Step::kUnplug) {
            plug_virtual(s.pad, s.kind == Step::kPlug);
            continue;
        }
        SDL_Joystick *j = g_virtual[s.pad].joystick;
        if (!j) {
            std::fprintf(stderr, "[pad] frame %llu: virtual controller %d is not plugged in\n", static_cast<unsigned long long>(now), s.pad);
            continue;
        }
        if (s.kind == Step::kAxis) SDL_JoystickSetVirtualAxis(j, s.control, raw_axis(s.control, s.value));
        else SDL_JoystickSetVirtualButton(j, s.control, SDL_PRESSED);
        g_holds.push_back(Hold{now + s.frames, s.pad, s.control, s.kind == Step::kAxis});
    }
}

// --- the key file and the key card ---

void apply_mouse_mode() {
    // a mouse that pushes a stick needs the pointer locked in the window, else it runs into the window's edge
    const bool want = g_window && g_keys.mouse_stick != 0 && !g_menu.is_open() && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != want) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    g_mouse_sum_x = g_mouse_sum_y = 0;
    g_mouse_vx = g_mouse_vy = 0.0;
}

void remember_keys_time() {
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (!error) g_keys_time = t;
}

// Reads keys.ini; writes it with the defaults when it is missing. A line that cannot be read is logged and ignored.
void load_keys() {
    std::string text;
    bool have = false;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
        have = true;
    } else if (std::FILE *out = ps2::os::open(g_keys_path, "w")) {
        const std::string defaults = keymap::default_file_text();
        std::fwrite(defaults.data(), 1, defaults.size(), out);
        std::fclose(out);
        std::fprintf(stderr, "[keys] wrote %s with the default keys\n", g_keys_path.c_str());
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the default keys are used and cannot be changed\n", g_keys_path.c_str());
    }
    keymap::Config config = keymap::defaults();
    std::vector<std::string> problems;
    if (have) keymap::parse(text, config, problems);
    for (const std::string &p : problems) std::fprintf(stderr, "[keys] %s, %s\n", g_keys_path.c_str(), p.c_str());
    g_keys = config;
    if (!g_look_forced && config.look >= 0) g_look = config.look;
    g_keys.look = g_look;
    ps2::set_video_options(static_cast<std::uint32_t>(g_look));
    // Hydrate live graphics, FPS, toast, and audio settings from keys.ini
    g_aspect_mode = std::clamp(config.aspect, 0, 2);
    g_brightness = std::clamp(config.brightness, -50, 50);
    g_contrast = std::clamp(config.contrast, 50, 150);
    g_saturation = std::clamp(config.saturation, 0, 200);
    g_gamma = std::clamp(config.gamma, 50, 200);
    g_scanlines = std::clamp(config.scanlines, 0, 100);
    g_deinterlace = config.deinterlace != 0;
    if (!config.fps_cap.empty()) g_fps_cap = config.fps_cap;
    g_fps_speed_sync = config.fps_speed_sync != 0;
    g_toast_theme = std::clamp(config.toast_theme, 0, static_cast<int>(achievements::ToastTheme::Count) - 1);
    if (!config.present_mode.empty()) {
        g_present_mode = config.present_mode;
#ifdef PS2_GPU
        if (g_gpu_present) ps2::gs::gpu::set_present_mode(g_present_mode);
#endif
    }
    g_master_vol = std::clamp(config.master_volume, 0, 100);
    g_muted = config.mute != 0;
    g_audio_device = config.audio_device;
    if (config.audio_stretch >= 0) g_audio_stretch = config.audio_stretch != 0;
    g_chime_vol.store(std::clamp(config.chime_volume, 0, 100), std::memory_order_relaxed);
    g_bgm_vol = std::clamp(config.bgm_volume, 0, 100);
    g_sfx_vol = std::clamp(config.sfx_volume, 0, 100);
    g_save_slot = std::clamp(config.save_slot, 0, 9);
    ps2::quick_set_active_slot(g_save_slot);
    g_slot_picker_on_quickkey = config.slot_picker_on_quickkey != 0;
    g_ssao = std::clamp(config.ssao, 0, 100);
    g_bloom = std::clamp(config.bloom, 0, 100);
    g_ssgi = std::clamp(config.ssgi, 0, 100);
    g_filmic = config.filmic != 0;
    g_vignette = std::clamp(config.vignette, 0, 100);
#ifdef PS2_GPU
    if (config.gpu_binning >= 0) {
        g_gpu_binning_on = config.gpu_binning != 0;
        ps2::gs::gpu::set_gpu_binning_enabled(g_gpu_binning_on);
    }
#endif
    apply_video_params();
    apply_audio_settings();
    remember_keys_time();
    std::fprintf(stderr, "[keys] %s: %s%zu problem%s; mouse stick %s\n", g_keys_path.c_str(), have ? "read, " : "defaults, ", problems.size(),
                 problems.size() == 1 ? "" : "s", g_keys.mouse_stick == 0 ? "off" : g_keys.mouse_stick == 1 ? "left" : "right");
    apply_mouse_mode();
}

// when the window gets the focus back, and when the key card opens: pick up changes the player made to keys.ini
void reload_keys_if_changed() {
    if (g_keys_path.empty()) return;
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (error || t != g_keys_time) load_keys();  // missing: written again with the defaults
}

#ifdef _WIN32
void edit_keys_file() {
    const std::wstring path = ps2::os::path(g_keys_path).wstring();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
#endif

// F1 (or the help key, H by default): the key card. A message box of the system. The buttons are the quick save and the quick load,
// for laptops where F5 and F8 need the Fn key.
//
// Windows and macOS: the box blocks the window thread while it is open (Windows keeps answering the window's messages inside the box;
// the game holds: the picture stands, the sound goes quiet, and it goes on where it was when the box closes).
//
// Everywhere else (Linux, the BSDs): the box runs on a thread of its own and the game keeps running behind it. SDL's box there is a
// separate program (zenity) or a window of the compositor, and the window thread must never wait for it: a box that is hidden by the
// compositor, opened on another workspace, or simply not noticed stopped the whole game for as long as it stood (the window thread
// answers the compositor, draws the pictures and takes the emulation's frames; GTA III froze 14 s that way on 2026-10-29,
// docs/linux.md "The window freeze"). The answer is taken in poll(), on the window thread.
#if !defined(_WIN32) && !defined(__APPLE__)
#define PS2_KEY_CARD_ON_A_THREAD 1
#endif

enum KeyCardChoice { kCardClose, kCardSave, kCardLoad, kCardEdit };

struct KeyCard {
    std::string text;
    std::atomic<int> state{0};  // 0 no box, 1 the box is open (its thread is in SDL_ShowMessageBox), 2 closed, the answer waits in poll()
    std::atomic<int> chosen{kCardClose};
    std::atomic<bool> failed{false};
    ps2::hitch::Clock::time_point opened{};
};
// never destroyed: a box that is still open when the program ends must not meet a destroyed object
KeyCard &g_card = *new KeyCard;
Uint32 g_focus_gained_ticks = 0;  // when the window last got the keyboard focus, for the log line when the card opens

void close_menu_if_open(const char *why);

// what happens after the box: the cursor back, the mouse mode again, the button that was chosen
void key_card_answer(int chosen) {
    SDL_ShowCursor(g_menu.is_open() ? SDL_ENABLE : SDL_DISABLE);
    apply_mouse_mode();
    if (chosen == kCardSave || chosen == kCardLoad) {
        close_menu_if_open("a button of the key card");  // the card was opened from the menu, which still holds the game
        g_note_wanted = true;
        ps2::quick_request(chosen == kCardSave ? ps2::QuickKey::save : ps2::QuickKey::load);
    }
#ifdef _WIN32
    else if (chosen == kCardEdit) edit_keys_file();
#endif
}

// the window thread, once per pass of poll(): the box on its thread has closed
void take_key_card_answer() {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 2) return;
    const double seconds = ps2::hitch::ms_since(g_card.opened) / 1000.0;
    if (!g_card.failed.load()) std::fprintf(stderr, "[keys] the key card was closed after %.1f s (the game ran on behind it)\n", seconds);
    const int chosen = g_card.chosen.load();
    g_card.state.store(0, std::memory_order_release);
    key_card_answer(chosen);
#endif
}

// why: the name of the key that opened it, for the log (how a card opens that nobody meant to open is the question this answers)
void show_key_card(const char *why) {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 0) return;  // one box at a time
#endif
    reload_keys_if_changed();
    g_card.text = keymap::card_text(g_keys);
    if (!g_keys_path.empty())
        g_card.text += "\nTo change keys: \"Change keys\" in the quick menu, or edit this file (the game reads it again when you come back):\n" + g_keys_path + "\n";
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    const Uint32 since_focus = SDL_GetTicks() - g_focus_gained_ticks;
    std::fprintf(stderr, "[keys] the key card opens (%s; the window got the keyboard focus %s)\n", why,
                 g_focus_gained_ticks && since_focus < 3000 ? (std::to_string(since_focus) + " ms before").c_str() : "more than 3 s before or never");
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the box needs the pointer
    SDL_ShowCursor(SDL_ENABLE);
    g_card.opened = ps2::hitch::Clock::now();
#ifdef PS2_KEY_CARD_ON_A_THREAD
    g_card.failed.store(false);
    g_card.chosen.store(kCardClose);
    g_card.state.store(1, std::memory_order_release);
    std::thread([] {
        SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
            {0, kCardSave, "Quick save (F5)"},
            {0, kCardLoad, "Quick load (F8)"},
        };
        SDL_MessageBoxData box{};
        box.flags = SDL_MESSAGEBOX_INFORMATION;
        box.window = nullptr;  // the box is not tied to the game window, whose thread is not the one that waits for it
        box.title = "Keys (F1)";
        box.message = g_card.text.c_str();
        box.numbuttons = static_cast<int>(std::size(buttons));
        box.buttons = buttons;
        int chosen = kCardClose;
        if (SDL_ShowMessageBox(&box, &chosen) != 0) {
            std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
            g_card.failed.store(true);
            chosen = kCardClose;
        }
        g_card.chosen.store(chosen);
        g_card.state.store(2, std::memory_order_release);
    }).detach();
#else
    SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
        {0, kCardSave, "Quick save (F5)"},
        {0, kCardLoad, "Quick load (F8)"},
#ifdef _WIN32
        {0, kCardEdit, "Edit keys.ini"},
#endif
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = g_window;
    box.title = "Keys (F1)";
    box.message = g_card.text.c_str();
    box.numbuttons = static_cast<int>(std::size(buttons));
    box.buttons = buttons;
    int chosen = kCardClose;
    if (SDL_ShowMessageBox(&box, &chosen) != 0) {
        std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
        chosen = kCardClose;
    }
    key_card_answer(chosen);
#endif
}

// A stick pushed by keys: all the way in a direction; two keys make a diagonal that is as far from the middle as a straight push
// (not 1.4 times as far, which the game would take as a push past the stick's circle).
void push_stick(int dx, int dy, std::uint8_t &x, std::uint8_t &y) {
    const bool diagonal = dx != 0 && dy != 0;
    if (dx) x = diagonal ? static_cast<std::uint8_t>(0x80 + dx * 90) : (dx < 0 ? 0x00 : 0xFF);
    if (dy) y = diagonal ? static_cast<std::uint8_t>(0x80 + dy * 90) : (dy < 0 ? 0x00 : 0xFF);
}

// the mouse as a stick: how fast it moves, sampled every 16 ms (the window thread runs more often than the game's frames)
void mouse_stick_value(std::uint8_t &x, std::uint8_t &y) {
    const Uint32 now = SDL_GetTicks();
    const Uint32 dt = now - g_mouse_sample;
    if (dt >= 16) {
        g_mouse_vx = g_mouse_vx * 0.5 + 0.5 * g_mouse_sum_x / dt;
        g_mouse_vy = g_mouse_vy * 0.5 + 0.5 * g_mouse_sum_y / dt;
        g_mouse_sum_x = g_mouse_sum_y = 0;
        g_mouse_sample = now;
    }
    const auto level = [&](double v) {
        double d = v * 60.0 * g_keys.mouse_speed;
        // past the game's own dead zone at once, so slow mouse moves still turn the camera
        if (std::abs(d) > 2.0) d += std::copysign(g_keys.mouse_deadzone * 127.0, d);
        return static_cast<std::uint8_t>(std::clamp(128.0 + d, 0.0, 255.0));
    };
    x = level(g_keys.mouse_invert & 1 ? -g_mouse_vx : g_mouse_vx);
    y = level(g_keys.mouse_invert & 2 ? -g_mouse_vy : g_mouse_vy);
}

// the keyboard and the mouse as the first controller
void read_keyboard(std::uint16_t &pressed, std::uint8_t &lx, std::uint8_t &ly, std::uint8_t &rx, std::uint8_t &ry) {
    static const std::uint16_t kDpad[] = {kUp, kDown, kLeft, kRight};
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    // mouse buttons only count while the window is the one in front (a click on another window is not for the game)
    const bool focus = !g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    const Uint32 mouse = focus ? SDL_GetMouseState(nullptr, nullptr) : 0;
    const Uint32 now = SDL_GetTicks();
    const auto down = [&](int input) {
        switch (input) {
        case keymap::kMouseLeft: return (mouse & SDL_BUTTON_LMASK) != 0;
        case keymap::kMouseMiddle: return (mouse & SDL_BUTTON_MMASK) != 0;
        case keymap::kMouseRight: return (mouse & SDL_BUTTON_RMASK) != 0;
        case keymap::kMouseX1: return (mouse & SDL_BUTTON_X1MASK) != 0;
        case keymap::kMouseX2: return (mouse & SDL_BUTTON_X2MASK) != 0;
        case keymap::kWheelUp: return static_cast<Sint32>(g_wheel_up_until - now) > 0;
        case keymap::kWheelDown: return static_cast<Sint32>(g_wheel_down_until - now) > 0;
        default: return input > 0 && input < SDL_NUM_SCANCODES && keys[input] != 0;
        }
    };
    const auto any = [&](int action) {
        for (int input : g_keys.inputs[action])
            if (down(input)) return true;
        return false;
    };
    for (int i = 0; i < 4; ++i)
        if (any(keymap::DpadUp + i)) pressed |= kDpad[i];
    static const struct {
        int action;
        std::uint16_t mask;
    } kButtons[] = {
        {keymap::Cross, kCross}, {keymap::Circle, kCircle}, {keymap::Square, kSquare}, {keymap::Triangle, kTriangle},
        {keymap::L1, kL1},       {keymap::R1, kR1},         {keymap::L2, kL2},         {keymap::R2, kR2},
        {keymap::L3, kL3},       {keymap::R3, kR3},         {keymap::Start, kStart},   {keymap::Select, kSelect},
    };
    for (const auto &b : kButtons)
        if (any(b.action)) pressed |= b.mask;
    if (any(keymap::Rage)) pressed |= kL3 | kR3;
    // two opposite keys together cancel each other
    const int ldx = (any(keymap::LeftRight) ? 1 : 0) - (any(keymap::LeftLeft) ? 1 : 0);
    const int ldy = (any(keymap::LeftDown) ? 1 : 0) - (any(keymap::LeftUp) ? 1 : 0);
    const int rdx = (any(keymap::RightRight) ? 1 : 0) - (any(keymap::RightLeft) ? 1 : 0);
    const int rdy = (any(keymap::RightDown) ? 1 : 0) - (any(keymap::RightUp) ? 1 : 0);
    push_stick(ldx, ldy, lx, ly);
    push_stick(rdx, rdy, rx, ry);
    if (ldx || ldy) {
        g_last_dx = ldx;
        g_last_dy = ldy;
    }
    // The roll: God of War's evade is a push of the right stick. While the roll key is held the walk keys push it too (the left
    // stick keeps its push, so Kratos faces the way he rolls); held with no walk key, it rolls the way he was last walking.
    if (any(keymap::Roll)) {
        const bool walking = ldx || ldy;
        push_stick(walking ? ldx : g_last_dx, walking ? ldy : g_last_dy, rx, ry);
    }
    if (g_keys.mouse_stick) {
        std::uint8_t mx = 0x80, my = 0x80;
        mouse_stick_value(mx, my);
        std::uint8_t &sx = g_keys.mouse_stick == 1 ? lx : rx, &sy = g_keys.mouse_stick == 1 ? ly : ry;
        if (sx == 0x80) sx = mx;
        if (sy == 0x80) sy = my;
    }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (port > 0 && plugged[port] != (pad != nullptr)) {
        plugged[port] = pad != nullptr;
        ps2::set_pad_connected(port, plugged[port]);
    }
    std::uint16_t pressed = 0;
    std::uint8_t l2 = 0xFF, r2 = 0xFF;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (port == 0) read_keyboard(pressed, lx, ly, rx, ry);
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (b.button != SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(pad, b.button)) pressed |= b.mask;
        // Select, then Start opens the quick menu, and the game must get neither. So the game gets this Select late: once it has been
        // held for 300 ms without Start, or as a short press when it is let go without Start. (Start first and then Select would
        // already have reached the game: the way is Select first.) A Select of the keyboard (Tab) is not held back.
        static Uint32 select_since[2] = {}, select_tap_until[2] = {};
        static bool select_combo[2] = {};
        const bool sel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK), st = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const Uint32 now = SDL_GetTicks();
        bool select_to_game = false;
        if (sel) {
            if (!select_since[port]) select_since[port] = now ? now : 1;
            if (st) select_combo[port] = true;
            select_to_game = !select_combo[port] && now - select_since[port] >= 300;
        } else {
            if (select_since[port] && !select_combo[port]) select_tap_until[port] = now + 80;
            select_since[port] = 0;
            select_combo[port] = false;
        }
        if (select_to_game || static_cast<Sint32>(select_tap_until[port] - now) > 0) pressed |= kSelect;
        if (sel && st) pressed &= ~kStart;
        const auto trigger = [&](SDL_GameControllerAxis a, std::uint16_t bit, std::uint8_t &pressure) {
            const int v = SDL_GameControllerGetAxis(pad, a);
            if (v <= 2000 || (pressed & bit)) return;
            pressed |= bit;
            pressure = static_cast<std::uint8_t>(std::max(1, v * 255 / 32767));
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, kL2, l2);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kR2, r2);
        stick(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, lx, ly);
        stick(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, rx, ry);
    }
    if (g_menu.is_open()) g_block_input[port] = true;
    if (g_block_input[port]) {
        if (!pressed) g_block_input[port] = false;  // all let go: from the next pass on the game gets this port again
        pressed = 0;
        lx = ly = rx = ry = 0x80;
        l2 = r2 = 0xFF;
    } else {
        rumble(port);  // while the game waits its rumble is not sent again, so it stops after 200 ms
    }
    static std::uint64_t last[2] = {~0ull, ~0ull};
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40) | (static_cast<std::uint64_t>(l2) << 48) | (static_cast<std::uint64_t>(r2) << 56);
    if (state == last[port]) return;
    last[port] = state;
    if (g_pad_log) {
        static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                               "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        std::string names;
        for (int i = 0; i < 16; ++i)
            if (pressed & (1u << i)) names += std::string(" ") + kNames[i];
        std::fprintf(stderr, "[pad] frame %llu port %d buttons %04X%s, left stick %u %u, right stick %u %u, l2 %u r2 %u\n",
                     static_cast<unsigned long long>(ps2::frames_run()), port + 1, pressed, names.empty() ? " none" : names.c_str(), lx, ly, rx, ry,
                     pressed & kL2 ? l2 : 0, pressed & kR2 ? r2 : 0);
    }
    ps2::set_pad_pressure(8, l2, port);
    ps2::set_pad_pressure(9, r2, port);
    ps2::set_pad_buttons(pressed, port);
    ps2::set_pad_axis(0, rx, port);
    ps2::set_pad_axis(1, ry, port);
    ps2::set_pad_axis(2, lx, port);
    ps2::set_pad_axis(3, ly, port);
}

// keys.ini with some lines changed (`change` maps the file's text to the new text, e.g. with keymap::with_look), the other lines as they are
void rewrite_keys_file(const std::function<std::string(const std::string &)> &change, const char *what) {
    if (g_keys_path.empty()) return;
    std::string text;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
    } else {
        text = keymap::default_file_text();
    }
    text = change(text);
    if (std::FILE *out = ps2::os::open(g_keys_path, "wb")) {
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
        remember_keys_time();
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the %s is kept until the game closes\n", g_keys_path.c_str(), what);
    }
}

// F2: the next look (step -1: the one before, from the quick menu). The title says which one, and the choice goes into keys.ini (the
// other lines of the file stay as they are).
void cycle_look(int step = 1) {
    g_look = (g_look + step + keymap::kLookCount) % keymap::kLookCount;
    g_keys.look = g_look;
    ps2::set_video_options(static_cast<std::uint32_t>(g_look));
    g_notice_text = std::string("Picture look (F2): ") + keymap::look_name(g_look);
    g_notice_until = SDL_GetTicks() + 4000;
    refresh_title();
    std::fprintf(stderr, "[video] picture look %d: %s\n", g_look, keymap::look_name(g_look));
    if (g_look_forced) return;
    rewrite_keys_file([](const std::string &t) { return keymap::with_look(t, g_look); }, "look");
}

// --- the quick menu ---

void toggle_fullscreen() {
    const bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void window_pixels(int &w, int &h) {
    w = h = 0;
    if (g_renderer) SDL_GetRendererOutputSize(g_renderer, &w, &h);
    else SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

std::string gpu_adapter_label(const std::string &spec) {
#ifdef PS2_GPU
    const std::string &act = ps2::gs::gpu::active_adapter();
    if (spec.empty() || spec == "auto") return act.empty() ? "Auto" : ("Auto (" + act + ")");
    if (spec == "low_power") return "Integrated / Low power";
    return spec;
#else
    return spec.empty() || spec == "auto" ? "Auto" : spec;
#endif
}

std::string gpu_api_label(const std::string &api) {
    if (api == "vulkan") return "Vulkan";
    if (api == "metal") return "Metal";
    return "Direct3D 12";
}

// Capture the game picture cleanly (NO menu or toast overlays)
bool capture_clean_game_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &w, std::uint32_t &h) {
#ifdef PS2_GPU
    if (g_gpu_present) return ps2::gs::gpu::capture_game_frame(rgba, w, h);
#endif
    if (!g_pixels.empty() && g_tex_w > 0 && g_tex_h > 0) {
        rgba = g_pixels;
        w = g_tex_w;
        h = g_tex_h;
        return true;
    }
    return ps2::copy_frame(rgba, w, h);
}

int max_scale_limit() {
#ifdef PS2_GPU
    return std::clamp(static_cast<int>(ps2::gs::gpu::max_safe_scale()), 1, 64);
#else
    return 8;
#endif
}

std::uint64_t estimate_scale_vram(int scale) {
#ifdef PS2_GPU
    return ps2::gs::gpu::estimate_vram_bytes(static_cast<std::uint32_t>(std::max(1, scale)));
#else
    const std::uint64_t s = static_cast<std::uint64_t>(std::max(1, scale));
    return 8ull * 1024ull * 1024ull * s * s;
#endif
}

// Makes the panel (the menu, or the note after it) for the window's size and hands it to the one who draws (or takes it away).
void menu_paint(bool show_again = true) {
    if (show_again) g_menu_redraw = true;
    g_overlay_on = g_menu.is_open() || !g_note_text.empty();
    if (!g_overlay_on) {
#ifdef PS2_GPU
        if (g_gpu_present) ps2::gs::gpu::set_overlay(nullptr, 0, 0, 0, 0);
#endif
        return;
    }
    int w = 0, h = 0;
    window_pixels(w, h);
    if (g_menu.is_open()) {
        g_display_hz = query_display_hz();
        quickmenu::Values v;
        v.look = keymap::look_name(g_look);
        v.scale_now = g_scale_now;
        v.scale_next = g_scale_next;
        v.scale_max = max_scale_limit();
        v.scale_locked = g_scale_locked;
        v.vram_bytes_next = estimate_scale_vram(g_scale_next);
        if (g_menu.typing_scale() && !g_menu.scale_input().empty()) {
            const int typed = std::clamp(std::atoi(g_menu.scale_input().c_str()), 1, v.scale_max);
            v.vram_bytes_custom = estimate_scale_vram(typed);
        }
        v.sharp_now = g_sharp_now;
        v.sharp_next = g_sharp_next;
        v.sharp_locked = g_sharp_locked;
        v.fps_cap_label = format_fps_cap_label();
        v.fps_speed_sync = g_fps_speed_sync;
#ifdef PS2_GPU
        v.present_mode = g_gpu_present ? ps2::gs::gpu::active_present_mode() : g_present_mode;
        g_gpu_submit_on = ps2::gs::gpu::submit_thread_enabled();
#else
        v.present_mode = g_present_mode;
#endif
        v.aspect_mode = g_aspect_mode;
        v.brightness = g_brightness;
        v.contrast = g_contrast;
        v.saturation = g_saturation;
        v.gamma = g_gamma;
        v.scanlines = g_scanlines;
        v.deinterlace = g_deinterlace;
        v.ssao = g_ssao;
        v.bloom = g_bloom;
        v.ssgi = g_ssgi;
        v.filmic = g_filmic;
        v.vignette = g_vignette;
        v.toast_theme = g_toast_theme;
        v.gpu_adapter = gpu_adapter_label(g_gpu_next);
        v.gpu_adapter_next = g_gpu_next != g_gpu_now;
        v.gpu_adapter_locked = g_gpu_locked;
        v.gpu_api = gpu_api_label(g_gpu_api_next);
        v.gpu_api_next = g_gpu_api_next != g_gpu_api_now;
        v.gpu_api_locked = g_gpu_api_locked;
        v.gpu_submit_on = g_gpu_submit_on;
#ifdef PS2_GPU
        g_gpu_binning_on = ps2::gs::gpu::gpu_binning_enabled();
#endif
        v.gpu_binning_on = g_gpu_binning_on;
        v.fullscreen = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
        v.active_slot = g_save_slot;
        v.slot_picker_on_quickkey = g_slot_picker_on_quickkey;
        if (g_menu.on_save_states_page()) {
            for (int i = 0; i < 10; ++i) v.slots[i] = ps2::quick_slot_info(i);
            const int sel = g_menu.selected_slot() >= 0 ? g_menu.selected_slot() : g_save_slot;
            if (sel != g_cached_thumb_slot) {
                g_cached_thumb_slot = sel;
                g_cached_thumb_rgba.clear();
                g_cached_thumb_w = g_cached_thumb_h = 0;
                if (v.slots[sel].has_thumb) ps2::quick_slot_thumbnail(sel, g_cached_thumb_rgba, g_cached_thumb_w, g_cached_thumb_h);
            }
            if (!g_cached_thumb_rgba.empty()) {
                v.slot_thumb_rgba = &g_cached_thumb_rgba;
                v.slot_thumb_w = g_cached_thumb_w;
                v.slot_thumb_h = g_cached_thumb_h;
            }
        }

        v.master_volume = g_master_vol;
        v.muted = g_muted;
        v.audio_device = g_audio_device.empty() ? "System Default" : g_audio_device;
        v.audio_stretch = g_audio_stretch;
        v.chime_volume = g_chime_vol.load(std::memory_order_relaxed);
        v.bgm_volume = g_bgm_vol;
        v.sfx_volume = g_sfx_vol;

        if (g_menu.on_keys_page()) {
            for (int a = 0; a < keymap::kChangeableCount; ++a) v.key_rows.push_back({keymap::action_label(a), keymap::keys_text(g_keys, a)});
            v.key_note = g_key_note;
        }
        std::vector<achievements::Entry> ach_snapshot;
        {
            std::lock_guard<std::mutex> lk(g_ach_mutex);
            v.ach_unlocked = g_achievements.unlocked_count();
            v.ach_total = g_achievements.total_count();
            if (g_menu.on_achievements_page()) ach_snapshot = g_achievements.entries();
        }
        if (g_menu.on_achievements_page()) v.ach_entries = &ach_snapshot;
        g_menu.draw(w, h, v, g_menu_picture);
    } else {
        quickmenu::Menu::draw_note(w, h, g_note_text, g_menu_picture);
    }
    const quickmenu::Picture &p = g_menu_picture;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::set_overlay(p.rgba.data(), static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height), static_cast<std::uint32_t>(p.x),
                                  static_cast<std::uint32_t>(p.y));
        return;
    }
#endif
    if (!g_renderer) return;
    if (!g_menu_texture || g_menu_texture_w != p.width || g_menu_texture_h != p.height) {
        if (g_menu_texture) SDL_DestroyTexture(g_menu_texture);
        // ABGR8888 is a packed format: red in the low byte of the 32-bit value, as the panel is made
        g_menu_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p.width, p.height);
        if (g_menu_texture) SDL_SetTextureBlendMode(g_menu_texture, SDL_BLENDMODE_BLEND);
        g_menu_texture_w = p.width;
        g_menu_texture_h = p.height;
    }
    if (g_menu_texture) SDL_UpdateTexture(g_menu_texture, nullptr, p.rgba.data(), p.width * 4);
}

// the SDL renderer: the panel and toast on top of the game's picture, in window pixels (the game's picture is drawn with a logical size)
void draw_menu_sdl() {
    if ((!g_overlay_on || !g_menu_texture) && (!g_toast_picture.active || !g_toast_texture)) return;
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(g_renderer, &lw, &lh);
    SDL_RenderSetLogicalSize(g_renderer, 0, 0);
    if (g_overlay_on && g_menu_texture) {
        const SDL_Rect place{g_menu_picture.x, g_menu_picture.y, g_menu_picture.width, g_menu_picture.height};
        SDL_RenderCopy(g_renderer, g_menu_texture, nullptr, &place);
    }
    if (g_toast_picture.active && g_toast_texture) {
        const SDL_Rect tplace{static_cast<int>(g_toast_picture.x), static_cast<int>(g_toast_picture.y), static_cast<int>(g_toast_picture.width),
                              static_cast<int>(g_toast_picture.height)};
        SDL_RenderCopy(g_renderer, g_toast_texture, nullptr, &tplace);
    }
    if (lw && lh) SDL_RenderSetLogicalSize(g_renderer, lw, lh);
}

// the last picture once more, with the panel as it is now: the game sends no new ones while it waits
void menu_show_again() {
    g_menu_redraw = false;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::present_again();
        return;
    }
#endif
    if (!g_renderer || !g_texture) return;
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void open_menu(const char *why) {
    if (g_menu.is_open()) return;
    reload_keys_if_changed();
    g_note_text.clear();
    g_key_note.clear();
    g_menu.open();
    g_menu.set_key_rows(keymap::kChangeableCount);
    g_menu_hold.store(true, std::memory_order_release);
    if (g_pipe) g_pipe->set_paused(true);
    g_menu_opened_ticks = SDL_GetTicks();
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the pointer can click the entries
    SDL_ShowCursor(SDL_ENABLE);
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    std::fprintf(stderr, "[menu] the quick menu opens (%s) at frame %llu; the game waits\n", why, static_cast<unsigned long long>(ps2::frames_run()));
    menu_paint();
}

void close_menu(const char *why) {
    if (!g_menu.is_open()) return;
    g_menu.close();
    g_block_input[0] = g_block_input[1] = true;
    menu_paint();  // takes the panel away (shown at once, before the game's next picture)
    SDL_ShowCursor(SDL_DISABLE);
    apply_mouse_mode();
    std::fprintf(stderr, "[menu] the quick menu closes (%s) after %.1f s; the game goes on\n", why, (SDL_GetTicks() - g_menu_opened_ticks) / 1000.0);
    if (g_pipe) g_pipe->set_paused(false);
    g_menu_hold.store(false, std::memory_order_release);
}

void close_menu_if_open(const char *why) { close_menu(why); }

void set_notice(const std::string &text) {
    g_notice_text = text;
    g_notice_until = SDL_GetTicks() + 5000;
    refresh_title();
}

void take_manual_screenshot() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!capture_clean_game_frame(rgba, w, h)) {
        set_notice("Screenshot failed: no game picture yet");
        if (g_beep_on) g_beep_request = 4;
        return;
    }
    std::filesystem::path saved;
    screenshot::save_game_async(std::move(rgba), w, h, &saved);
    if (saved.empty()) {
        set_notice("Screenshot failed: could not write PNG");
        if (g_beep_on) g_beep_request = 4;
        return;
    }
    const std::string fname = ps2::os::utf8(saved.filename());
    set_notice("Screenshot saved: " + fname);
    if (g_beep_on) g_beep_request = 1;
    if (!g_menu.is_open()) {
        g_note_text = "Screenshot saved";
        g_note_until = SDL_GetTicks() + 2000;
        menu_paint(false);
    }
}

void poll_achievements_emulation() {
    std::lock_guard<std::mutex> lk(g_ach_mutex);
    g_achievements.poll_ram(ps2::disc::active_disc() + 1);
}

void tick_achievement_toast() {
    int w = 0, h = 0;
    window_pixels(w, h);
    if (w <= 0 || h <= 0) return;
    achievements::ToastTickResult res;
    {
        std::lock_guard<std::mutex> lk(g_ach_mutex);
        res = g_achievements.tick_toast(audio_seconds(), w, h, g_toast_theme, g_toast_picture);
    }
    if (res.chime && g_beep_on) g_beep_request = 5;
    if (res.changed) {
#ifdef PS2_GPU
        if (g_gpu_present) {
            if (g_toast_picture.active && !g_toast_picture.rgba.empty()) {
                ps2::gs::gpu::set_toast_overlay(g_toast_picture.rgba.data(), g_toast_picture.width, g_toast_picture.height, g_toast_picture.x,
                                                g_toast_picture.y);
            } else {
                ps2::gs::gpu::set_toast_overlay(nullptr, 0, 0, 0, 0);
            }
        }
#endif
        if (g_renderer) {
            if (g_toast_picture.active && !g_toast_picture.rgba.empty()) {
                if (!g_toast_texture || g_toast_texture_w != static_cast<int>(g_toast_picture.width) ||
                    g_toast_texture_h != static_cast<int>(g_toast_picture.height)) {
                    if (g_toast_texture) SDL_DestroyTexture(g_toast_texture);
                    g_toast_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                        static_cast<int>(g_toast_picture.width), static_cast<int>(g_toast_picture.height));
                    if (g_toast_texture) SDL_SetTextureBlendMode(g_toast_texture, SDL_BLENDMODE_BLEND);
                    g_toast_texture_w = static_cast<int>(g_toast_picture.width);
                    g_toast_texture_h = static_cast<int>(g_toast_picture.height);
                }
                if (g_toast_texture) SDL_UpdateTexture(g_toast_texture, nullptr, g_toast_picture.rgba.data(), static_cast<int>(g_toast_picture.width * 4));
            }
        }
        if (g_menu.is_open()) g_menu_redraw = true;
    }
    if (res.take_screenshot && g_toast_picture.active && !g_toast_picture.rgba.empty()) {
        std::vector<std::uint32_t> game_rgba;
        std::uint32_t gw = 0, gh = 0;
        if (capture_clean_game_frame(game_rgba, gw, gh)) {
            screenshot::save_achievement_async(res.screenshot_title, std::move(game_rgba), gw, gh, g_toast_picture.rgba, g_toast_picture.width,
                                               g_toast_picture.height, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), g_toast_picture.x,
                                               g_toast_picture.y);
        }
    }
}

// the lines of keys.ini for these buttons, written with the keys now in force (comments and every other line stay as they are)
void write_key_lines(const std::vector<int> &actions) {
    if (actions.empty()) return;
    rewrite_keys_file(
        [&](const std::string &text) {
            std::string out = text;
            for (int a : actions) out = keymap::with_inputs(out, a, g_keys.inputs[a]);
            return out;
        },
        "new keys");
}

// the key card's and the menu's idea of a mouse button
int mouse_input(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return keymap::kMouseLeft;
    case SDL_BUTTON_MIDDLE: return keymap::kMouseMiddle;
    case SDL_BUTTON_RIGHT: return keymap::kMouseRight;
    case SDL_BUTTON_X1: return keymap::kMouseX1;
    case SDL_BUTTON_X2: return keymap::kMouseX2;
    default: return 0;
    }
}

// The keys page waits for a key: the next key or mouse button (wheel too) is the highlighted button's new key. Esc or a controller button
// cancels. Always true for the events that belong to input (nothing reaches the game).
bool capture_event(const SDL_Event &e) {
    int input = 0;
    switch (e.type) {
    case SDL_KEYDOWN:
        if (e.key.repeat) return true;
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_menu.stop_capture();
            g_key_note = "Nothing changed.";
            menu_paint();
            return true;
        }
        input = e.key.keysym.scancode;
        break;
    case SDL_MOUSEBUTTONDOWN: input = mouse_input(e.button.button); break;
    case SDL_MOUSEWHEEL: input = e.wheel.y > 0 ? keymap::kWheelUp : e.wheel.y < 0 ? keymap::kWheelDown : 0; break;
    case SDL_CONTROLLERBUTTONDOWN:
        g_menu.stop_capture();
        g_key_note = "Nothing changed.";
        menu_paint();
        return true;
    case SDL_KEYUP: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION: return true;
    default: return false;
    }
    if (input == 0) return true;
    std::vector<int> changed;
    std::string message;
    const int action = g_menu.key_row();
    if (!keymap::assign_input(g_keys, action, input, changed, message)) {
        g_key_note = message;  // (it stays waiting)
    } else {
        g_menu.stop_capture();
        g_key_note = message;
        write_key_lines(changed);
        std::fprintf(stderr, "[menu] keys: %s\n", message.c_str());
    }
    menu_paint();
    return true;
}

void save_setting(const char *key, const std::string &value, const char *what) {
    rewrite_keys_file([&](const std::string &t) { return keymap::with_setting(t, key, value); }, what);
}

void apply_new_scale(int new_scale) {
    const int max_s = max_scale_limit();
    g_scale_next = std::clamp(new_scale, 1, max_s);
    g_keys.scale = g_scale_next;
    rewrite_keys_file([](const std::string &t) { return keymap::with_scale(t, g_scale_next); }, "resolution");
    set_notice("Resolution " + std::to_string(g_scale_next) + "x (~" + quickmenu::Menu::format_bytes(estimate_scale_vram(g_scale_next)) + ")" +
               (g_scale_next == g_scale_now ? "" : " - applies on next start"));
    std::fprintf(stderr, "[menu] resolution %dx for the next start (this run: %dx, GPU safe max: %dx)\n", g_scale_next, g_scale_now, max_s);
}

void capture_slot_thumbnail() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (capture_clean_game_frame(rgba, w, h)) ps2::quick_set_pending_thumbnail(rgba.data(), w, h);
}

void set_active_save_slot(int slot, bool show_pill) {
    g_save_slot = std::clamp(slot, 0, 9);
    g_keys.save_slot = g_save_slot;
    ps2::quick_set_active_slot(g_save_slot);
    save_setting("save_slot", std::to_string(g_save_slot), "save slot");
    if (show_pill) {
        const ps2::SaveSlotInfo info = ps2::quick_slot_info(g_save_slot);
        std::string msg = "Save Slot " + std::to_string(g_save_slot);
        if (!info.exists) msg += " (Empty)";
        else if (!info.area.empty()) msg += " (" + info.area + (info.max_level > 0 ? (" Lv" + std::to_string(info.max_level)) : "") + ")";
        set_notice(msg);
        if (!g_menu.is_open()) {
            g_note_text = msg;
            g_note_until = SDL_GetTicks() + 1800;
            menu_paint(false);
        }
    }
}

void open_save_picker_menu(quickmenu::SavePickerMode mode, const char *why) {
    if (!g_menu.is_open()) {
        reload_keys_if_changed();
        g_note_text.clear();
        g_key_note.clear();
        g_menu.open_save_picker(mode, g_save_slot);
        g_menu.set_key_rows(keymap::kChangeableCount);
        g_menu_hold.store(true, std::memory_order_release);
        if (g_pipe) g_pipe->set_paused(true);
        g_menu_opened_ticks = SDL_GetTicks();
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
        g_cached_thumb_slot = -1;
        std::fprintf(stderr, "[menu] save picker opens (%s)\n", why);
    } else {
        g_menu.open_save_picker(mode, g_save_slot);
        g_cached_thumb_slot = -1;
    }
    menu_paint();
}

void do_slot_save(int slot, bool from_menu) {
    set_active_save_slot(slot, false);
    capture_slot_thumbnail();
    g_cached_thumb_slot = -1;
    if (from_menu) close_menu("Save Slot");
    g_note_wanted = true;
    ps2::quick_request_slot(ps2::QuickKey::save, g_save_slot);
}

void do_slot_load(int slot, bool from_menu) {
    set_active_save_slot(slot, false);
    if (from_menu) close_menu("Load Slot");
    g_note_wanted = true;
    ps2::quick_request_slot(ps2::QuickKey::load, g_save_slot);
}

// what an entry does; step is -1 or 1 for left and right (Enter and a click go forward)
void menu_command(quickmenu::Command c, int step) {
    using quickmenu::Command;
    switch (c) {
    case Command::none: menu_paint(); return;
    case Command::open_settings:
    case Command::open_achievements: menu_paint(); return;
    case Command::open_save_states:
        g_cached_thumb_slot = -1;
        g_menu.select_slot(g_save_slot);
        menu_paint();
        return;
    case Command::resume: close_menu("Resume"); return;
    case Command::save:
        do_slot_save(g_save_slot, true);
        return;
    case Command::load:
        do_slot_load(g_save_slot, true);
        return;
    case Command::slot_save: {
        const int s = g_menu.selected_slot();
        if (s >= 0) {
            const auto info = ps2::quick_slot_info(s);
            if (info.exists && !g_menu.arm_slot_save()) {
                set_notice("Press again to overwrite Slot " + std::to_string(s));
                menu_paint();
                return;
            }
            g_menu.clear_slot_armed();
            do_slot_save(s, true);
        }
        return;
    }
    case Command::slot_load: {
        const int s = g_menu.selected_slot();
        if (s >= 0) {
            const auto info = ps2::quick_slot_info(s);
            if (!info.exists && !(s == 0 && !ps2::quick_newest_state().empty())) {
                set_notice("Slot " + std::to_string(s) + " is empty");
                if (g_beep_on) g_beep_request = 4;
                menu_paint();
                return;
            }
            g_menu.clear_slot_armed();
            do_slot_load(s, true);
        }
        return;
    }
    case Command::slot_delete: {
        const int s = g_menu.selected_slot();
        if (s >= 0) {
            const auto info = ps2::quick_slot_info(s);
            if (!info.exists) {
                set_notice("Slot " + std::to_string(s) + " is already empty");
                break;
            }
            if (!g_menu.arm_slot_delete()) {
                set_notice("Press Del/Y again to delete Slot " + std::to_string(s));
                break;
            }
            if (ps2::quick_delete_slot(s)) {
                g_cached_thumb_slot = -1;
                set_notice("Deleted Slot " + std::to_string(s));
                if (g_beep_on) g_beep_request = 3;
            }
        }
        break;
    }
    case Command::screenshot:
        take_manual_screenshot();
        break;
    case Command::scale_pow2: {
        if (g_scale_locked) {
            set_notice("The resolution is set by PS2_SCALE; remove it to choose here");
            break;
        }
        const int max_s = max_scale_limit();
        std::vector<int> pows;
        for (int s = 1; s <= max_s && s <= 64; s *= 2) pows.push_back(s);
        if (pows.empty()) pows.push_back(1);
        int idx = 0;
        for (std::size_t i = 0; i < pows.size(); ++i)
            if (pows[i] <= g_scale_next) idx = static_cast<int>(i);
        const int n = static_cast<int>(pows.size());
        idx = (idx + step % n + n) % n;
        apply_new_scale(pows[idx]);
        break;
    }
    case Command::scale_custom_begin:
        if (g_scale_locked) {
            set_notice("The resolution is set by PS2_SCALE; remove it to choose here");
            break;
        }
        g_menu.begin_scale_input(g_scale_next);
        break;
    case Command::fps_cap: {
        static const char *const kCaps[] = {"30", "60", "120", "144", "240", "display", "unlimited"};
        constexpr int kCount = static_cast<int>(std::size(kCaps));
        int idx = 1;
        for (int i = 0; i < kCount; ++i)
            if (g_fps_cap == kCaps[i]) idx = i;
        idx = (idx + step % kCount + kCount) % kCount;
        g_fps_cap = kCaps[idx];
        g_keys.fps_cap = g_fps_cap;
        g_display_hz = query_display_hz();
        apply_video_params();
        g_last.store(0, std::memory_order_relaxed);
        save_setting("fps_cap", g_fps_cap, "FPS limit");
        set_notice("FPS limit: " + format_fps_cap_label());
        break;
    }
    case Command::fps_speed_sync:
        g_fps_speed_sync = !g_fps_speed_sync;
        g_keys.fps_speed_sync = g_fps_speed_sync ? 1 : 0;
        apply_video_params();
        save_setting("fps_speed_sync", g_fps_speed_sync ? "on" : "off", "High-FPS speed sync");
        set_notice(std::string("High-FPS speed sync: ") + (g_fps_speed_sync ? "on (real-time audio)" : "off (unlocked speed)"));
        break;
    case Command::present_mode: {
#ifdef PS2_GPU
        if (g_gpu_present) {
            const auto &modes = ps2::gs::gpu::supported_present_modes();
            if (!modes.empty()) {
                const std::string cur = ps2::gs::gpu::active_present_mode();
                int idx = 0;
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (modes[i] == cur) idx = static_cast<int>(i);
                const int n = static_cast<int>(modes.size());
                idx = (idx + step % n + n) % n;
                g_present_mode = modes[idx];
                ps2::gs::gpu::set_present_mode(g_present_mode);
                g_keys.present_mode = g_present_mode;
                save_setting("present_mode", g_present_mode, "VSync / Present mode");
                set_notice("VSync / Present mode: " + g_present_mode);
                break;
            }
        }
#endif
        set_notice("VSync / Present mode is only adjustable on WebGPU");
        break;
    }
    case Command::look: cycle_look(step); break;
    case Command::sharp_2d:
        if (g_sharp_locked) {
            set_notice("Text and menus are set by PS2_SHARP_2D; remove it to choose here");
            break;
        }
        g_sharp_next = (g_sharp_next + step + 3) % 3;
        g_keys.sharp_2d = g_sharp_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_sharp_2d(t, g_sharp_next); }, "2D art filter");
        set_notice(std::string("Text and menus: ") + keymap::sharp_2d_word(g_sharp_next) + (g_sharp_next == g_sharp_now ? "" : ", used when the game starts the next time"));
        std::fprintf(stderr, "[menu] 2D art %s for the next start (this run: %s)\n", keymap::sharp_2d_word(g_sharp_next), keymap::sharp_2d_word(g_sharp_now));
        break;
    case Command::aspect: {
        static const char *const kWords[] = {"4:3", "16:9", "stretch"};
        g_aspect_mode = (g_aspect_mode + step % 3 + 3) % 3;
        g_keys.aspect = g_aspect_mode;
        apply_video_params();
        save_setting("aspect", kWords[g_aspect_mode], "aspect ratio");
        set_notice(std::string("Aspect ratio: ") + kWords[g_aspect_mode]);
        break;
    }
    case Command::brightness:
        g_brightness = std::clamp(g_brightness + step * 5, -50, 50);
        g_keys.brightness = g_brightness;
        apply_video_params();
        save_setting("brightness", std::to_string(g_brightness), "brightness");
        break;
    case Command::contrast:
        g_contrast = std::clamp(g_contrast + step * 5, 50, 150);
        g_keys.contrast = g_contrast;
        apply_video_params();
        save_setting("contrast", std::to_string(g_contrast), "contrast");
        break;
    case Command::saturation:
        g_saturation = std::clamp(g_saturation + step * 5, 0, 200);
        g_keys.saturation = g_saturation;
        apply_video_params();
        save_setting("saturation", std::to_string(g_saturation), "saturation");
        break;
    case Command::gamma:
        g_gamma = std::clamp(g_gamma + step * 5, 50, 200);
        g_keys.gamma = g_gamma;
        apply_video_params();
        save_setting("gamma", std::to_string(g_gamma), "gamma");
        break;
    case Command::scanlines:
        g_scanlines = std::clamp(g_scanlines + step * 10, 0, 100);
        g_keys.scanlines = g_scanlines;
        apply_video_params();
        save_setting("scanlines", std::to_string(g_scanlines), "CRT scanlines");
        break;
    case Command::deinterlace:
        g_deinterlace = !g_deinterlace;
        g_keys.deinterlace = g_deinterlace ? 1 : 0;
        apply_video_params();
        save_setting("deinterlace", g_deinterlace ? "on" : "off", "deinterlacing");
        break;
    case Command::ssao:
        g_ssao = std::clamp(g_ssao + step * 10, 0, 100);
        g_keys.ssao = g_ssao;
        apply_video_params();
        save_setting("ssao", std::to_string(g_ssao), "SSAO");
        break;
    case Command::bloom:
        g_bloom = std::clamp(g_bloom + step * 10, 0, 100);
        g_keys.bloom = g_bloom;
        apply_video_params();
        save_setting("bloom", std::to_string(g_bloom), "Bloom");
        break;
    case Command::ssgi:
        g_ssgi = std::clamp(g_ssgi + step * 10, 0, 100);
        g_keys.ssgi = g_ssgi;
        apply_video_params();
        save_setting("ssgi", std::to_string(g_ssgi), "SSGI");
        break;
    case Command::filmic:
        g_filmic = !g_filmic;
        g_keys.filmic = g_filmic ? 1 : 0;
        apply_video_params();
        save_setting("filmic", g_filmic ? "on" : "off", "Filmic tone map");
        break;
    case Command::vignette:
        g_vignette = std::clamp(g_vignette + step * 10, 0, 100);
        g_keys.vignette = g_vignette;
        apply_video_params();
        save_setting("vignette", std::to_string(g_vignette), "Vignette");
        break;
    case Command::toast_theme: {
        constexpr int kThemes = static_cast<int>(achievements::ToastTheme::Count);
        g_toast_theme = (g_toast_theme + step % kThemes + kThemes) % kThemes;
        g_keys.toast_theme = g_toast_theme;
        save_setting("toast_theme", achievements::toast_theme_word(g_toast_theme), "toast gradient theme");
        set_notice(std::string("Toast gradient: ") + achievements::toast_theme_name(g_toast_theme));
        break;
    }
    case Command::gpu_adapter: {
        if (g_gpu_locked) {
            set_notice("The graphics chip is set by PS2_GPU_ADAPTER; remove it to choose here");
            break;
        }
        std::vector<std::string> choices = {"auto"};
#ifdef PS2_GPU
        for (const std::string &a : ps2::gs::gpu::known_adapters())
            if (!a.empty() && std::find(choices.begin(), choices.end(), a) == choices.end()) choices.push_back(a);
#endif
        choices.push_back("low_power");
        int idx = 0;
        for (std::size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == g_gpu_next) idx = static_cast<int>(i);
        const int n = static_cast<int>(choices.size());
        idx = (idx + step % n + n) % n;
        g_gpu_next = choices[idx];
        g_keys.gpu = g_gpu_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_gpu(t, g_gpu_next); }, "graphics chip");
        set_notice("Graphics chip: " + gpu_adapter_label(g_gpu_next) + (g_gpu_next == g_gpu_now ? "" : ", used when the game starts the next time"));
        std::fprintf(stderr, "[menu] graphics chip \"%s\" for the next start (this run: \"%s\")\n", g_gpu_next.c_str(), g_gpu_now.c_str());
        break;
    }
    case Command::gpu_api:
        if (g_gpu_api_locked) {
            set_notice("The graphics API is set by PS2_GPU_BACKEND; remove it to choose here");
            break;
        }
#ifdef _WIN32
        g_gpu_api_next = (g_gpu_api_next == "vulkan") ? "dx12" : "vulkan";
        g_keys.gpu_api = g_gpu_api_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_gpu_api(t, g_gpu_api_next); }, "graphics API");
        set_notice("Graphics API: " + gpu_api_label(g_gpu_api_next) + (g_gpu_api_next == g_gpu_api_now ? "" : ", used when the game starts the next time"));
        std::fprintf(stderr, "[menu] graphics API %s for the next start (this run: %s)\n", gpu_api_label(g_gpu_api_next).c_str(), gpu_api_label(g_gpu_api_now).c_str());
#else
        set_notice("DirectX 12 is Windows-only; using native " + gpu_api_label(g_gpu_api_now));
#endif
        break;
    case Command::gpu_submit:
        g_gpu_submit_on = !g_gpu_submit_on;
        g_keys.gpu_submit_thread = g_gpu_submit_on ? 1 : 0;
#ifdef PS2_GPU
        ps2::gs::gpu::set_submit_thread_enabled(g_gpu_submit_on);
#endif
        rewrite_keys_file([](const std::string &t) { return keymap::with_gpu_submit_thread(t, g_gpu_submit_on); }, "async GPU submit");
        set_notice(std::string("Async GPU submit: ") + (g_gpu_submit_on ? "on" : "off") + " (active now)");
        std::fprintf(stderr, "[menu] async GPU submit thread switched %s live\n", g_gpu_submit_on ? "on" : "off");
        break;
    case Command::gpu_binning:
        g_gpu_binning_on = !g_gpu_binning_on;
        g_keys.gpu_binning = g_gpu_binning_on ? 1 : 0;
#ifdef PS2_GPU
        ps2::gs::gpu::set_gpu_binning_enabled(g_gpu_binning_on);
#endif
        save_setting("gpu_binning", g_gpu_binning_on ? "on" : "off", "GPU tile binning");
        set_notice(std::string("GPU tile binning: ") + (g_gpu_binning_on ? "on" : "off") + " (active now)");
        break;
    case Command::slot_picker_toggle:
        g_slot_picker_on_quickkey = !g_slot_picker_on_quickkey;
        g_keys.slot_picker_on_quickkey = g_slot_picker_on_quickkey ? 1 : 0;
        save_setting("slot_picker_on_quickkey", g_slot_picker_on_quickkey ? "on" : "off", "slot picker on F5/F8");
        set_notice(std::string("F5/F8 opens slot picker: ") + (g_slot_picker_on_quickkey ? "on" : "off"));
        break;
    case Command::fullscreen: toggle_fullscreen(); break;
    case Command::reset_graphics:
        g_aspect_mode = 0;
        g_brightness = 0;
        g_contrast = 100;
        g_saturation = 100;
        g_gamma = 100;
        g_scanlines = 0;
        g_deinterlace = true;
        g_ssao = 0;
        g_bloom = 0;
        g_ssgi = 0;
        g_filmic = false;
        g_vignette = 0;
        apply_video_params();
        rewrite_keys_file([](const std::string &t) {
            std::string s = keymap::with_setting(t, "aspect", "4:3");
            s = keymap::with_setting(s, "brightness", "0");
            s = keymap::with_setting(s, "contrast", "100");
            s = keymap::with_setting(s, "saturation", "100");
            s = keymap::with_setting(s, "gamma", "100");
            s = keymap::with_setting(s, "scanlines", "0");
            s = keymap::with_setting(s, "deinterlace", "on");
            s = keymap::with_setting(s, "ssao", "0");
            s = keymap::with_setting(s, "bloom", "0");
            s = keymap::with_setting(s, "ssgi", "0");
            s = keymap::with_setting(s, "filmic", "off");
            s = keymap::with_setting(s, "vignette", "0");
            return s;
        }, "reset graphics");
        set_notice("Image adjustments & lighting reset to defaults");
        break;
    case Command::master_volume:
        g_master_vol = std::clamp(g_master_vol + step * 5, 0, 100);
        g_keys.master_volume = g_master_vol;
        apply_audio_settings();
        save_setting("master_volume", std::to_string(g_master_vol), "master volume");
        break;
    case Command::mute:
        g_muted = !g_muted;
        g_keys.mute = g_muted ? 1 : 0;
        apply_audio_settings();
        save_setting("mute", g_muted ? "on" : "off", "mute");
        break;
    case Command::audio_device: {
        std::vector<std::string> devs = {""};
        const int count = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < count; ++i) {
            if (const char *name = SDL_GetAudioDeviceName(i, 0))
                if (*name && std::find(devs.begin(), devs.end(), name) == devs.end()) devs.emplace_back(name);
        }
        int idx = 0;
        for (std::size_t i = 0; i < devs.size(); ++i)
            if (devs[i] == g_audio_device) idx = static_cast<int>(i);
        const int n = static_cast<int>(devs.size());
        idx = (idx + step % n + n) % n;
        g_audio_device = devs[idx];
        g_keys.audio_device = g_audio_device;
        reopen_audio_device(g_audio_device);
        save_setting("audio_device", g_audio_device.empty() ? "default" : g_audio_device, "audio device");
        set_notice("Audio output: " + (g_audio_device.empty() ? std::string("System Default") : g_audio_device));
        break;
    }
    case Command::audio_stretch:
        g_audio_stretch = !g_audio_stretch;
        g_keys.audio_stretch = g_audio_stretch ? 1 : 0;
        apply_audio_settings();
        save_setting("audio_stretch", g_audio_stretch ? "on" : "off", "audio time-stretch");
        break;
    case Command::chime_volume: {
        const int next = std::clamp(g_chime_vol.load(std::memory_order_relaxed) + step * 5, 0, 100);
        g_chime_vol.store(next, std::memory_order_relaxed);
        g_keys.chime_volume = next;
        save_setting("chime_volume", std::to_string(next), "chime volume");
        if (g_beep_on && next > 0) g_beep_request = 1;
        break;
    }
    case Command::bgm_volume:
        g_bgm_vol = std::clamp(g_bgm_vol + step * 5, 0, 100);
        g_keys.bgm_volume = g_bgm_vol;
        apply_audio_settings();
        save_setting("bgm_volume", std::to_string(g_bgm_vol), "music & voices volume");
        break;
    case Command::sfx_volume:
        g_sfx_vol = std::clamp(g_sfx_vol + step * 5, 0, 100);
        g_keys.sfx_volume = g_sfx_vol;
        apply_audio_settings();
        save_setting("sfx_volume", std::to_string(g_sfx_vol), "sound effects volume");
        break;
    case Command::change_keys: g_key_note.clear(); break;  // (the menu has switched to its keys page)
    case Command::key_edit: g_key_note.clear(); break;     // (the highlighted line waits for a key now)
    case Command::key_defaults: {
        std::vector<int> changed;
        keymap::restore_default_inputs(g_keys, changed);
        write_key_lines(changed);
        g_key_note = changed.empty() ? "These are the default keys already." : "The default keys are back.";
        std::fprintf(stderr, "[menu] the default keys are back (%zu button%s changed)\n", changed.size(), changed.size() == 1 ? "" : "s");
        break;
    }
    case Command::keys:
        // the menu stays open behind the card (the game keeps holding); the card's own buttons close it when they save or load
        show_key_card("Keys in the quick menu");
        break;
    case Command::quit:
        std::fprintf(stderr, "[menu] Quit the game: the memory card is written, the game ends\n");
        ps2::flush_memory_card();  // right away, as when the window is closed
        ps2::halt("quit from the quick menu");
        close_menu("Quit the game");  // a game that waits for the menu must go on to see the halt
        return;
    }
    menu_paint();
}

bool is_help_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Help];
    return std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

bool is_menu_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Menu];
    return k == SDL_SCANCODE_F1 || std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

// the pointer in window pixels (events can be in the SDL renderer's logical units; the mouse state is not)
void menu_pointer_pixels(int &px, int &py) {
    int x = 0, y = 0, ww = 1, wh = 1, pw = 0, ph = 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    window_pixels(pw, ph);
    px = x * pw / std::max(ww, 1);
    py = y * ph / std::max(wh, 1);
}

bool menu_pointer_select() {
    int px = 0, py = 0;
    menu_pointer_pixels(px, py);
    return g_menu.select_at(px, py);
}

// Keys, buttons and the mouse for the menu. True when the event was the menu's (while it is open every key and button is).
bool menu_event(const SDL_Event &e) {
    using quickmenu::Command;
    if (!g_menu.is_open()) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F12) {
            take_manual_screenshot();
            return true;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && is_menu_key(e.key.keysym.scancode)) {
            open_menu((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode)).c_str());
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            SDL_GameController *pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
            const int b = e.cbutton.button;
            const bool both = pad && ((b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) ||
                                      (b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)));
            if (b == SDL_CONTROLLER_BUTTON_GUIDE || both) {
                open_menu(b == SDL_CONTROLLER_BUTTON_GUIDE ? "the controller's Guide button" : "Select + Start on the controller");
                return true;
            }
        }
        return false;
    }
    const auto side = [](int step) {
        if (g_menu.on_achievements_page()) {
            g_menu.cycle_ach_status(step);
            menu_paint();
            return;
        }
        menu_command(g_menu.side(), step);
    };
    const auto move = [](int step) {
        g_menu.move(step);
        menu_paint();
    };
    // Esc, Circle, the right mouse button: one page back on sub-pages, else the menu closes
    const auto back = [](const char *why) {
        if (g_menu.back()) menu_paint();
        else close_menu(why);
    };
    if (g_menu.capturing()) return capture_event(e);
    if (g_menu.typing_scale()) {
        if (e.type == SDL_KEYDOWN) {
            const SDL_Scancode k = e.key.keysym.scancode;
            if (k == SDL_SCANCODE_ESCAPE) {
                g_menu.cancel_scale_input();
                menu_paint();
            } else if (k == SDL_SCANCODE_BACKSPACE) {
                g_menu.backspace_scale_digit();
                menu_paint();
            } else if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) {
                const int val = g_menu.finish_scale_input(1, max_scale_limit());
                apply_new_scale(val);
                menu_paint();
            } else if (k >= SDL_SCANCODE_1 && k <= SDL_SCANCODE_9) {
                g_menu.push_scale_digit(static_cast<char>('1' + (k - SDL_SCANCODE_1)));
                menu_paint();
            } else if (k == SDL_SCANCODE_0 || k == SDL_SCANCODE_KP_0) {
                g_menu.push_scale_digit('0');
                menu_paint();
            } else if (k >= SDL_SCANCODE_KP_1 && k <= SDL_SCANCODE_KP_9) {
                g_menu.push_scale_digit(static_cast<char>('1' + (k - SDL_SCANCODE_KP_1)));
                menu_paint();
            }
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                g_menu.cancel_scale_input();
                menu_paint();
            } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                const int val = g_menu.finish_scale_input(1, max_scale_limit());
                apply_new_scale(val);
                menu_paint();
            }
            return true;
        }
        if (e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) return true;
        return false;
    }
    switch (e.type) {
    case SDL_KEYDOWN: {
        const SDL_Scancode k = e.key.keysym.scancode;
        if (!e.key.repeat && is_menu_key(k)) {
            close_menu("its key");
            return true;
        }
        switch (k) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: move(-1); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: move(1); break;
        case SDL_SCANCODE_X:
            if (!e.key.repeat && g_menu.on_save_states_page()) menu_command(Command::slot_save, 1);
            break;
        case SDL_SCANCODE_L:
            if (!e.key.repeat && g_menu.on_save_states_page()) menu_command(Command::slot_load, 1);
            break;
        case SDL_SCANCODE_DELETE:
            if (!e.key.repeat && g_menu.on_save_states_page()) menu_command(Command::slot_delete, 1);
            break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: side(-1); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: side(1); break;
        case SDL_SCANCODE_Q:
            if (g_menu.on_settings_page()) {
                g_menu.switch_tab(-1);
                menu_paint();
            } else if (g_menu.on_achievements_page()) {
                g_menu.cycle_ach_tier(-1);
                menu_paint();
            }
            break;
        case SDL_SCANCODE_E:
            if (g_menu.on_settings_page()) {
                g_menu.switch_tab(1);
                menu_paint();
            } else if (g_menu.on_achievements_page()) {
                g_menu.cycle_ach_tier(1);
                menu_paint();
            }
            break;
        case SDL_SCANCODE_TAB:
            if (!e.key.repeat && g_menu.on_achievements_page()) {
                g_menu.toggle_reveal_secrets();
                menu_paint();
            }
            break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) menu_command(g_menu.choose(), 1);
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            if (!e.key.repeat) back(k == SDL_SCANCODE_ESCAPE ? "Esc" : "Backspace");
            break;
        // the program's keys keep working; those that need the game running close the menu first
        case SDL_SCANCODE_F2: if (!e.key.repeat) menu_command(Command::look, 1); break;
        case SDL_SCANCODE_F5:
            if (!e.key.repeat) menu_command(g_menu.on_save_states_page() ? Command::slot_save : Command::save, 1);
            break;
        case SDL_SCANCODE_F6:
            if (!e.key.repeat) {
                set_active_save_slot((g_save_slot + 9) % 10, false);
                if (g_menu.on_save_states_page()) g_menu.select_slot(g_save_slot);
                menu_paint();
            }
            break;
        case SDL_SCANCODE_F7:
            if (!e.key.repeat) {
                set_active_save_slot((g_save_slot + 1) % 10, false);
                if (g_menu.on_save_states_page()) g_menu.select_slot(g_save_slot);
                menu_paint();
            }
            break;
        case SDL_SCANCODE_F8: if (!e.key.repeat) menu_command(Command::load, 1); break;
        case SDL_SCANCODE_F9:
            if (!e.key.repeat) {
                close_menu("F9");
                ps2::quick_request(ps2::QuickKey::report);
            }
            break;
        case SDL_SCANCODE_F11: if (!e.key.repeat) menu_command(Command::fullscreen, 1); break;
        case SDL_SCANCODE_F12: if (!e.key.repeat) take_manual_screenshot(); break;
        default:
            if (!e.key.repeat && g_menu.on_save_states_page()) {
                int digit = -1;
                if (k >= SDL_SCANCODE_1 && k <= SDL_SCANCODE_9) digit = 1 + (k - SDL_SCANCODE_1);
                else if (k == SDL_SCANCODE_0 || k == SDL_SCANCODE_KP_0) digit = 0;
                else if (k >= SDL_SCANCODE_KP_1 && k <= SDL_SCANCODE_KP_9) digit = 1 + (k - SDL_SCANCODE_KP_1);
                if (digit >= 0) {
                    g_menu.select_slot(digit);
                    if (g_menu.save_picker_mode() == quickmenu::SavePickerMode::save_pick) menu_command(Command::slot_save, 1);
                    else if (g_menu.save_picker_mode() == quickmenu::SavePickerMode::load_pick) menu_command(Command::slot_load, 1);
                    else menu_paint();
                    break;
                }
            }
            if (!e.key.repeat && is_help_key(k)) menu_command(Command::keys, 1);
            break;
        }
        return true;
    }
    case SDL_KEYUP: return true;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: side(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: side(1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (g_menu.on_settings_page()) {
                g_menu.switch_tab(-1);
                menu_paint();
            } else if (g_menu.on_achievements_page()) {
                g_menu.cycle_ach_tier(-1);
                menu_paint();
            }
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (g_menu.on_settings_page()) {
                g_menu.switch_tab(1);
                menu_paint();
            } else if (g_menu.on_achievements_page()) {
                g_menu.cycle_ach_tier(1);
                menu_paint();
            }
            break;
        case SDL_CONTROLLER_BUTTON_X:
            if (g_menu.on_save_states_page()) menu_command(Command::slot_save, 1);
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            if (g_menu.on_achievements_page()) {
                g_menu.toggle_reveal_secrets();
                menu_paint();
            } else if (g_menu.on_save_states_page()) {
                menu_command(Command::slot_delete, 1);
            }
            break;
        case SDL_CONTROLLER_BUTTON_A: menu_command(g_menu.choose(), 1); break;  // Cross (the bottom button)
        case SDL_CONTROLLER_BUTTON_B: back("Circle"); break;              // Circle (the right one)
        case SDL_CONTROLLER_BUTTON_GUIDE: close_menu("the Guide button"); break;
        case SDL_CONTROLLER_BUTTON_START: close_menu("Start"); break;
        default: break;
        }
        return true;
    case SDL_CONTROLLERBUTTONUP: return true;
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            // the left stick moves the highlight once per push
            int &was = g_stick_step[g_pads[1] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[1])) == e.caxis.which ? 1 : 0];
            const int now = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : (std::abs(e.caxis.value) < 8000 ? 0 : was);
            if (now && now != was) move(now);
            was = now;
        }
        return true;
    case SDL_MOUSEMOTION:
        if ((e.motion.xrel || e.motion.yrel) && menu_pointer_select()) menu_paint();  // only a pointer that moved (not one that the window moved under)
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
            int px = 0, py = 0;
            menu_pointer_pixels(px, py);
            Command cmd = Command::none;
            if (g_menu.click_at(px, py, cmd)) menu_command(cmd, 1);
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            back("the right mouse button");
        }
        return true;
    case SDL_MOUSEBUTTONUP: return true;
    case SDL_MOUSEWHEEL:
        if (e.wheel.y) move(e.wheel.y > 0 ? -1 : 1);
        return true;
    default: return false;
    }
}

void show_notice() {
    ps2::QuickNotice n;
    if (ps2::quick_notice(n)) {
        g_notice_text = n.text;
        g_notice_until = SDL_GetTicks() + (n.kind == 0 ? 5000u : 6000u);
        if (g_note_wanted && n.kind > 0) {  // the answer to a save or load that the menu asked for: shown over the picture for a moment
            g_note_wanted = false;
            if (!g_menu.is_open()) {
                g_note_text = n.text;
                g_note_until = SDL_GetTicks() + 2500;
                menu_paint(false);
            }
        }
        if (n.kind == 2 && g_pipe) g_pipe->flush();  // the sound from before the load (the tone comes on top of the game's sound, so it stays)
        if (g_beep_on && n.kind > 0) g_beep_request = n.kind;
        refresh_title();
    } else if (!g_notice_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_notice_until) >= 0) {
        g_notice_text.clear();
        refresh_title();
    }
    if (!g_note_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_note_until) >= 0) {
        g_note_text.clear();
        if (!g_menu.is_open()) menu_paint(false);
    }
}

// PS2_HITCH_LOG=<ms>: SDL_PollEvent also pumps the system's messages and looks for controllers; say when it, or the handling of
// one event, took long (docs/hitches.md).
bool timed_poll_event(SDL_Event &e) {
    window_stage("asking the system for window events (SDL_PollEvent)");
    if (!ps2::hitch::on()) return SDL_PollEvent(&e) != 0;
    const auto t0 = ps2::hitch::Clock::now();
    const bool got = SDL_PollEvent(&e) != 0;
    const double ms = ps2::hitch::ms_since(t0);
    if (ms > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] SDL_PollEvent took %.1f ms (it returned %s, event type 0x%x)%s", ms, got ? "an event" : "nothing", got ? e.type : 0u, "\n");
    return got;
}

struct EventTimer {
    Uint32 type;
    ps2::hitch::Clock::time_point t0 = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    ~EventTimer() {
        if (!ps2::hitch::on()) return;
        const double ms = ps2::hitch::ms_since(t0);
        if (ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] handling the event of type 0x%x took %.1f ms%s", type, ms, "\n");
    }
};

void poll() {
    window_stage("window thread: starting a pass of poll()");
    if (!g_script.empty()) play_pad_script();
    ps2::quick_watchdog();
    take_key_card_answer();
    SDL_Event e;
    while (timed_poll_event(e)) {
        window_stage("handling a window event");
        const EventTimer event_timer{e.type};
        if (e.type == SDL_QUIT) {
            ps2::flush_memory_card();  // right away: a save made a moment ago must not wait for the emulation to wind down
            ps2::halt("window closed");
            close_menu("the window was closed");  // a game that waits for the menu must go on to see the halt
        }
        if (menu_event(e)) continue;
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) toggle_fullscreen();
        // F1 the quick menu (menu_event), F5 quick save, F8 quick load, F9 "something is wrong here" (the game's own buttons: keys.ini,
        // host/keymap.cpp); a help key of the player's own shows the key card at once
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (is_help_key(e.key.keysym.scancode))
                show_key_card((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode) + " went down").c_str());
            if (e.key.keysym.scancode == SDL_SCANCODE_F2) cycle_look();
            if (e.key.keysym.scancode == SDL_SCANCODE_F5) {
                if (g_slot_picker_on_quickkey) open_save_picker_menu(quickmenu::SavePickerMode::save_pick, "F5");
                else do_slot_save(g_save_slot, false);
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F6) {
                set_active_save_slot((g_save_slot + 9) % 10, true);
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F7) {
                set_active_save_slot((g_save_slot + 1) % 10, true);
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F8) {
                if (g_slot_picker_on_quickkey) open_save_picker_menu(quickmenu::SavePickerMode::load_pick, "F8");
                else do_slot_load(g_save_slot, false);
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F9) {
                ps2::quick_request(ps2::QuickKey::report);
            }
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_EXPOSED) && g_overlay_on)
            menu_paint();  // the panel for the new size
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                g_focus_gained_ticks = SDL_GetTicks();
                reload_keys_if_changed();
            }
            apply_mouse_mode();
        }
        if (e.type == SDL_MOUSEMOTION && g_keys.mouse_stick) {
            g_mouse_sum_x += e.motion.xrel;
            g_mouse_sum_y += e.motion.yrel;
        }
        if (e.type == SDL_MOUSEWHEEL) {
            // a turn of the wheel is a press of about a tenth of a second, which the game sees for a few frames
            (e.wheel.y > 0 ? g_wheel_up_until : g_wheel_down_until) = SDL_GetTicks() + 100;
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
        if (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))
            std::fprintf(stderr, "[pad] %s is not used: SDL knows no button layout for it (docs/sound-pad.md: gamecontrollerdb.txt)\n",
                         or_unknown(SDL_JoystickNameForIndex(e.jdevice.which)));
    }
    window_stage("reading the controllers");
    const auto after_events = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    poll_port(0);
    poll_port(1);
    show_notice();
    tick_achievement_toast();
    if (g_menu_redraw) menu_show_again();
    if (ps2::hitch::on() && ps2::hitch::ms_since(after_events) > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] reading the controllers and the notice took %.1f ms%s", ps2::hitch::ms_since(after_events), "\n");
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(w) * (ps2::g_options.widescreen ? 9 : 3) / (ps2::g_options.widescreen ? 16 : 4));
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, rgba, static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void present() {
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) show_pixels(g_pixels.data(), w, h);
}

double audio_seconds() { return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency()); }

double queued_ms() { return g_pipe ? g_pipe->queued_ms() : 0.0; }  // the game's sound waiting for the device, in milliseconds of game time

void write_audio_dumps();
void note_audio();

// PS2_NO_LIMIT=1 (docs/speed.md): a window run goes as fast as it can, with no wait for the sound device and no wait for the clock, to
// measure what the processor threads and the graphics chip can do. The sound plays out of step (it is dropped when it runs too far
// behind). Off unless set; "0" counts as off.
bool no_limit() {
    static const bool on = [] {
        const std::string value = ps2::os::env("PS2_NO_LIMIT");
        const bool set = !value.empty() && value != "0";
        if (set) std::fprintf(stderr, "[pace] PS2_NO_LIMIT is set: the game runs with no speed limit; the sound plays out of step\n");
        return set;
    }();
    return on;
}

// PS2_PACE=audio: the sound device paces the game (the game waits while more than ~80 ms of sound is waiting), as before. A device
// that takes its sound in large blocks (wireless headsets over WASAPI) then makes the frames uneven: seven of 20 ms, one of 30 ms, one
// of 3 ms, about six times a second (measured with Urban Reign and a Corsair HS80), which shows as judder in every camera pan even
// at a steady 50 fps. By default the clock paces the game instead, one frame every 1/50 or 1/60 s, and the sound only steers that
// clock: frames up to 1% longer while more sound than kSteerMs is waiting, up to 1% shorter while less. The sound stretcher still
// covers a game that runs slow.
bool audio_paced() {
    static const bool on = ps2::os::env("PS2_PACE") == "audio";
    return on;
}

constexpr double kSteerMs = 90.0;  // the sound the clock-paced game keeps waiting for the device: enough that a device taking large blocks does not dip it under the stretcher's gap guard (~45 ms with a 20 ms frame), which would stretch the sound for no reason

// Waits until the performance counter reaches `target`: sleeps while more than 2 ms are left, then spins, for a frame edge within
// a few microseconds (SDL_Delay alone wakes up to a millisecond late, which is the unevenness this is meant to remove).
void wait_until(std::uint64_t target) {
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    for (;;) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target) return;
        const double left_ms = static_cast<double>(target - now) * 1000.0 / static_cast<double>(frequency);
        if (left_ms > 2.0) SDL_Delay(static_cast<Uint32>(left_ms - 1.5));
        else std::this_thread::yield();
    }
}

void pace() {
    if (no_limit()) return;
    const double base_hz = ps2::g_options.pal ? 50.0 : 60.0;
    double target_hz = g_pace_target_hz.load(std::memory_order_relaxed);
    const bool speed_sync = g_pace_speed_sync.load(std::memory_order_relaxed);
    const bool audio_on = g_audio.load(std::memory_order_acquire) != 0;
    if (target_hz <= 0.0) {  // Unlimited FPS: still keep audio pipe & stats updated
        if (audio_on && g_pipe) {
            g_pipe->frame(audio_seconds());
            note_audio();
        }
        return;
    }
    // Hold 60/50 Hz during FMVs when High-FPS speed sync is active so cutscenes & voices never desync
    if (speed_sync && target_hz > base_hz && ps2::fmv_active()) target_hz = base_hz;
    const bool keep_audio_sync = speed_sync || target_hz <= base_hz + 1.0;

    static bool stuck = false;  // the device took no sound for a second; the clock paces the game until it takes some again
    if (audio_on && !audio_paced() && !stuck) {
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double steer = keep_audio_sync ? std::clamp((g_pipe->queued_ms() - kSteerMs) / kSteerMs * 0.02, -0.01, 0.01) : 0.0;
        const auto period = static_cast<std::uint64_t>(frequency / target_hz * (1.0 + steer));
        const std::uint64_t now = SDL_GetPerformanceCounter();
        std::uint64_t last = g_last.load(std::memory_order_relaxed);
        // more than two frames behind (a load, a hold): start counting again from now rather than rushing to catch up
        if (last && now < last + 2 * period) {
            last += period;
            g_last.store(last, std::memory_order_relaxed);
            wait_until(last);
        } else {
            g_last.store(now, std::memory_order_relaxed);
        }
        // a device that takes far less than the game makes (or none at all) must still hold the game: the old wait, as a ceiling
        const double hold = audio_seconds();
        if (keep_audio_sync) {
            while (g_pipe->queued_ms() > 3.0 * kSteerMs && audio_seconds() - hold < 1.0) SDL_Delay(1);
        }
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (keep_audio_sync && audio_seconds() - hold >= 1.0) {
            std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
            stuck = true;
        }
        return;
    }
    if (audio_on && keep_audio_sync && !(stuck && g_pipe->full())) {
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is waiting. The pipe times the game between
        // the frames (how much it slows down decides how much the sound is stretched) and does not count this wait as slowness.
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        while (g_pipe->full() && audio_seconds() - started < 1.0) SDL_Delay(1);  // at most a second: a stopped device must not hold the game
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (!stuck && waited >= 1.0) std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
        stuck = waited >= 1.0;
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const auto period = static_cast<std::uint64_t>(static_cast<double>(frequency) / target_hz);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    std::uint64_t last = g_last.load(std::memory_order_relaxed);
    if (last && now < last + 2 * period) {
        last += period;
        g_last.store(last, std::memory_order_relaxed);
        wait_until(last);
    } else {
        g_last.store(now, std::memory_order_relaxed);
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME &t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 1e7;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double resident_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0.0;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    static double cpu_start = 0.0, emu_start = 0.0, gpu_start = 0.0;
    static const double logical_cores = static_cast<double>(std::max(1u, std::thread::hardware_concurrency()));
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) {
        start = now;
        cpu_start = process_cpu_seconds();
        emu_start = g_emu_cpu_ms.load(std::memory_order_relaxed);
        gpu_start = ps2::gpu_busy_ms();
    }
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    const double cpu = process_cpu_seconds(), emu = g_emu_cpu_ms.load(std::memory_order_relaxed), gpu = ps2::gpu_busy_ms();
    const double total_cpu_pct = std::clamp((cpu - cpu_start) / seconds * 100.0 / logical_cores, 0.0, 100.0);
    const double main_cpu_pct = std::clamp((emu - emu_start) / (seconds * 10.0), 0.0, 100.0);
    char title[224];
    const double queued = queued_ms();
    char stretched[40] = "";  // "audio 62 ms" is the game's sound waiting; when it is played slower, by how much
    if (g_pipe && g_pipe->tempo() < 0.995) std::snprintf(stretched, sizeof stretched, ", stretched x%.1f", 1.0 / g_pipe->tempo());
    std::snprintf(title, sizeof title, "ps2-native-recomp  %.1f fps  CPU %.0f%% (main %.0f%%)  GPU %.0f%%  RAM %.0f MB  audio %.0f ms%s",
                  static_cast<double>(frames) / seconds, total_cpu_pct, main_cpu_pct, (gpu - gpu_start) / (seconds * 10.0), resident_mb(), queued, stretched);
    g_speed_title = title;
    refresh_title();
    start = now;
    frames = 0;
    cpu_start = cpu;
    emu_start = emu;
    gpu_start = gpu;
}

// PS2_PRESENT_LOG=1: once a second, how long the emulation thread waited per frame for the window thread (post_present: the
// window thread is two pictures behind) and for the sound (pace), and how the window thread's time was spent (docs/speed.md).
struct WaitLog {
    using Clock = std::chrono::steady_clock;
    bool on = std::getenv("PS2_PRESENT_LOG") != nullptr;
    Clock::time_point since = Clock::now();
    double post = 0, sound = 0, seen_working = 0, seen_idle = 0;
    std::atomic<double> working{0}, idle{0};  // written by the window thread, read by the emulation thread
    std::uint32_t frames = 0;
    static double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }
    void emulation_frame() {
        if (++frames < 60) return;
        const double seconds = std::chrono::duration<double>(Clock::now() - since).count();
        std::fprintf(stderr, "[waits] %.1f frames/s; emulation thread per frame: waiting for the window thread %.2f ms, for the sound %.2f ms; window thread per second: rendering %.0f ms, no work %.0f ms\n",
                     frames / seconds, post / frames, sound / frames, (working.load() - seen_working) / seconds, (idle.load() - seen_idle) / seconds);
        since = Clock::now();
        post = sound = 0;
        seen_working = working.load();
        seen_idle = idle.load();
        frames = 0;
    }
};
WaitLog g_waits;

// PS2_FRAME_LOG=<file>: one line per emulated frame, to find what happens when a scene suddenly gets slow (docs/speed.md, the arena).
// Columns: frame number, milliseconds since the previous frame, CPU milliseconds all the threads used meanwhile, milliseconds the emulation
// thread waited for the window thread and for the sound, shapes drawn, vector programs run by the slow interpreter, sound queued (ms).
struct FrameLog {
    std::FILE *file = nullptr;
    std::uint64_t last_counter = 0, last_prims = 0, last_interpreted = 0;
    double last_cpu = 0;
    FrameLog() {
        const std::string path = ps2::os::env("PS2_FRAME_LOG");
        if (!path.empty()) {
            file = ps2::os::open(path, "w");
            if (file) std::fprintf(file, "frame wall_ms cpu_ms wait_window_ms wait_sound_ms shapes interpreted audio_ms\n");
        }
    }
    void frame(double wait_window_ms, double wait_sound_ms, double queued_ms) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        const double cpu = process_cpu_seconds();
        const std::uint64_t prims = ps2::primitives_drawn(), interpreted = ps2::vu1_interpreted_calls();
        if (last_counter)
            std::fprintf(file, "%llu %.2f %.2f %.2f %.2f %llu %llu %.0f\n", static_cast<unsigned long long>(ps2::frames_run()),
                         static_cast<double>(now - last_counter) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency()), (cpu - last_cpu) * 1000.0,
                         wait_window_ms, wait_sound_ms, static_cast<unsigned long long>(prims - last_prims), static_cast<unsigned long long>(interpreted - last_interpreted), queued_ms);
        last_counter = now;
        last_cpu = cpu;
        last_prims = prims;
        last_interpreted = interpreted;
        if (ps2::frames_run() % 60 == 0) std::fflush(file);
    }
};
FrameLog g_frame_log;

void on_vblank() {
    g_emu_cpu_ms.store(ps2::hitch::thread_cpu_ms(), std::memory_order_relaxed);
    poll_achievements_emulation();
    show_speed();
    poll();
    present();
    pace();
    if (g_frame_log.file) g_frame_log.frame(0.0, 0.0, queued_ms());
    if (g_menu.is_open()) {  // the quick menu: this thread is the window's too, so it answers the window while the game waits
        const double since = audio_seconds();
        while (g_menu.is_open()) {
            poll();
            SDL_Delay(10);
        }
        if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
        g_last.store(0, std::memory_order_relaxed);
    }
}

// The quick menu with threads: the emulation thread waits here, at the end of its frame, until the menu closes.
void hold_for_menu() {
    if (!g_menu_hold.load(std::memory_order_acquire)) return;
    const double since = audio_seconds();
    while (g_menu_hold.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
    g_last.store(0, std::memory_order_relaxed);
}

// PS2_AUDIO_DUMP=<file.wav>: the sound exactly as the stretcher hands it to the sound device (no silence where it ran short),
// PS2_AUDIO_HEARD=<file.wav>: what the speaker got, silence included. 48000 Hz 16-bit stereo. The header is brought up to date
// about once a second, so a run that is killed keeps a file that plays up to there.
class WavOut {
public:
    ~WavOut() { close(); }
    bool open(const std::string &path) {
        file_ = ps2::os::open(path, "wb");
        if (!file_) std::fprintf(stderr, "[audio] cannot write %s\n", path.c_str());
        path_ = path;
        if (file_) header();
        return file_ != nullptr;
    }
    void write(const std::int16_t *samples, std::size_t frames) {
        if (!file_ || frames_ + frames > (0xFFFFFFFFull - 36) / 4) return;
        std::fwrite(samples, 4, frames, file_);
        frames_ += frames;
        if (frames_ - synced_ >= 48000) header();
    }
    void close() {
        if (!file_) return;
        header();
        std::fclose(file_);
        file_ = nullptr;
        std::fprintf(stderr, "[audio] wrote %llu samples (%.2f s) to %s\n", static_cast<unsigned long long>(frames_), static_cast<double>(frames_) / 48000.0, path_.c_str());
    }

private:
    void header() {
        const std::uint32_t bytes = static_cast<std::uint32_t>(frames_ * 4);
        const std::uint32_t words[] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16, 0x00020001u, 48000, 48000 * 4, 0x00100004u, 0x61746164u, bytes};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(words, 4, sizeof words / 4, file_);
        std::fseek(file_, 0, SEEK_END);
        std::fflush(file_);
        synced_ = frames_;
    }
    std::FILE *file_ = nullptr;
    std::string path_;
    std::uint64_t frames_ = 0, synced_ = 0;
};

WavOut g_dump, g_heard;

// PS2_AUDIO_PUSHLOG=<file>: when the sound chip pushed its sound and when each video frame ended (time in seconds, kind and size, waiting
// ms, frame), written at the end. tests/stretch_test.cpp replay <file> runs the sound pipe on the real timing (docs/sound.md).
struct PushLog {
    struct Row {
        double time;
        int kind, frames;
        float waiting;
        unsigned frame;
    };
    std::vector<Row> rows;
    std::string path = ps2::os::env("PS2_AUDIO_PUSHLOG");
    void add(int kind, int frames, double now) {
        if (!path.empty()) rows.push_back({now, kind, frames, static_cast<float>(queued_ms()), static_cast<unsigned>(ps2::frames_run())});
    }
    void write() {
        if (path.empty()) return;
        if (std::FILE *f = ps2::os::open(path, "w")) {
            for (const Row &r : rows) std::fprintf(f, "%.6f %d %.1f %u\n", r.time, r.kind * 100000 + r.frames, static_cast<double>(r.waiting), r.frame);
            std::fclose(f);
        }
        path.clear();
    }
};
PushLog g_pushlog;

struct Tone {
    int hz, ms;
};
constexpr Tone kBeepSaved[] = {{1047, 90}};
constexpr Tone kBeepLoaded[] = {{784, 80}, {0, 40}, {1047, 80}};
constexpr Tone kBeepReport[] = {{1047, 70}, {0, 40}, {784, 70}, {0, 40}, {523, 110}};
constexpr Tone kBeepTrouble[] = {{196, 300}};
constexpr Tone kBeepAchievement[] = {{659, 95}, {831, 95}, {988, 110}, {1319, 260}};
bool g_beep_running = false;

// Adds the tone that was asked for to the game's sound (48000 Hz stereo), in place. Runs on the sound device's thread.
void mix_beep(std::int16_t *samples, std::size_t frames) {
    static const Tone *seq = nullptr;
    static std::size_t count = 0, index = 0;
    static int left = 0, total = 0;
    static double phase = 0.0;
    static bool harmonic = false;
    if (const int kind = g_beep_request.exchange(0)) {
        harmonic = (kind == 5);
        seq = kind == 1 ? kBeepSaved : kind == 2 ? kBeepLoaded : kind == 3 ? kBeepReport : kind == 5 ? kBeepAchievement : kBeepTrouble;
        count = kind == 1 ? std::size(kBeepSaved) : kind == 2 ? std::size(kBeepLoaded) : kind == 3 ? std::size(kBeepReport) : kind == 5 ? std::size(kBeepAchievement) : std::size(kBeepTrouble);
        index = 0;
        total = left = seq[0].ms * 48;
        phase = 0.0;
    }
    const double vol = static_cast<double>(std::clamp(g_chime_vol.load(std::memory_order_relaxed), 0, 100)) / 100.0;
    g_beep_running = seq != nullptr;
    for (std::size_t i = 0; seq && i < frames; ++i) {
        while (left == 0) {
            if (++index >= count) {
                seq = nullptr;
                g_beep_running = false;
                return;
            }
            total = left = seq[index].ms * 48;
            phase = 0.0;
        }
        const int position = total - left--;
        if (!seq[index].hz) continue;
        const double ramp = std::min({1.0, position / 240.0, left / 480.0});
        const double wave = harmonic ? (0.72 * std::sin(phase) + 0.22 * std::sin(phase * 2.0) + 0.06 * std::sin(phase * 3.0)) : std::sin(phase);
        const int v = static_cast<int>(wave * ramp * (harmonic ? 4200.0 : 3500.0) * vol);
        phase += 2.0 * 3.14159265358979 * seq[index].hz / 48000.0;
        for (int ch = 0; ch < 2; ++ch) samples[i * 2 + ch] = static_cast<std::int16_t>(std::clamp(samples[i * 2 + ch] + v, -32768, 32767));
    }
}

bool reopen_audio_device(const std::string &device_name) {
    if (const SDL_AudioDeviceID old = g_audio.exchange(0, std::memory_order_acq_rel)) {
        SDL_CloseAudioDevice(old);
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    const char *dev = device_name.empty() ? nullptr : device_name.c_str();
    SDL_AudioDeviceID opened = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
    bool fell_back = false;
    if (!opened && dev) {
        std::fprintf(stderr, "[audio] could not open \"%s\" (%s); falling back to System Default\n", dev, SDL_GetError());
        opened = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (opened) {
            fell_back = true;
            g_audio_device.clear();
            g_keys.audio_device.clear();
        }
    }
    if (!opened) {
        std::fprintf(stderr, "[audio] no sound device: %s\n", SDL_GetError());
        return false;
    }
    g_audio.store(opened, std::memory_order_release);
    if (g_pipe) g_pipe->flush();
    SDL_PauseAudioDevice(opened, 0);
    ps2::set_audio_sink(queue_audio);
    std::fprintf(stderr, "[audio] active output device: %s\n", (fell_back || device_name.empty()) ? "System Default" : device_name.c_str());
    return true;
}

// The sound chip's batches of 32 frames arrive here on the emulation thread.
void queue_audio(const std::int16_t *samples, std::size_t frames) {
    const double now = audio_seconds();
    g_pushlog.add(0, static_cast<int>(frames), now);
    g_pipe->push(samples, frames, now);
}

// The sound device asks for its next block (its own thread): the pipe hands over the game's sound, stretched when the game is slow,
// and the tone for F5, F8 and F9 is mixed on top.
void audio_callback(void *, Uint8 *stream, int length) {
    static thread_local bool named = false;  // the sound device's own thread
    if (!named) {
        named = true;
        ps2::crash::enter_thread(ps2::crash::Thread::Sound);
    }
    ps2::crash::test_poll(ps2::crash::Thread::Sound);
    g_pipe->render(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(length) / 4, audio_seconds(), mix_beep);
}

void write_audio_dumps() {
    static std::vector<std::int16_t> chunk;
    if (g_pipe->device_tap.on) {
        chunk.clear();
        g_pipe->device_tap.take(chunk);
        g_dump.write(chunk.data(), chunk.size() / 2);
    }
    if (g_pipe->heard_tap.on) {
        chunk.clear();
        g_pipe->heard_tap.take(chunk);
        g_heard.write(chunk.data(), chunk.size() / 2);
    }
}

// once per video frame (from pace): the dump files, and a line when the sound had to stop for lack of sound
void note_audio() {
    static std::uint64_t seen = 0, last_report = 0;
    write_audio_dumps();
    g_pushlog.add(1, 0, audio_seconds());
    const std::uint64_t dry = g_pipe->stats().dry;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (dry > seen && (!last_report || now - last_report > 5 * SDL_GetPerformanceFrequency())) {
        std::fprintf(stderr, "[audio] the sound ran dry at frame %llu (%llu times so far): the game runs slower than real time%s\n", static_cast<unsigned long long>(ps2::frames_run()),
                     static_cast<unsigned long long>(dry), g_pipe->config().stretch ? " than the sound can be stretched to" : "");
        last_report = now;
    }
    seen = dry;
}

// at the end of a window run: how much sound the game made, how it was played, and how much silence there was
void finish_audio() {
    const SDL_AudioDeviceID dev = g_audio.exchange(0, std::memory_order_acq_rel);
    if (dev && g_pipe) {
        SDL_CloseAudioDevice(dev);  // the device thread stops here
        write_audio_dumps();
        const ps2sound::PipeStats s = g_pipe->stats();
        const double wall = s.first >= 0.0 ? s.last - s.first : 0.0;
        std::fprintf(stderr, "[audio] the device got %.2f s of sound in %.2f s; the queue ran dry %llu times (about %.2f s of silence in between), %llu samples dropped\n",
                     static_cast<double>(s.pushed) / 48000.0, wall, static_cast<unsigned long long>(s.dry), static_cast<double>(s.silence) / 48000.0,
                     static_cast<unsigned long long>(s.dropped));
        if (g_pipe->config().stretch)
            std::fprintf(stderr, "[audio] stretching: %.2f s of game sound played as %.2f s, %.2f s of that stretched (slowest tempo x%.2f; %.2f s at the limit of x%.1f, where it runs dry like without stretching)\n",
                         s.consumed / 48000.0, static_cast<double>(s.played) / 48000.0, static_cast<double>(s.stretched) / 48000.0, 1.0 / s.slowest,
                         static_cast<double>(s.at_cap) / 48000.0, g_pipe->config().max_ratio);
        else
            std::fprintf(stderr, "[audio] stretching is off (PS2_AUDIO_STRETCH=0)\n");
        g_pushlog.write();
        g_dump.close();
        g_heard.close();
    }
}

}

void poll_input() { poll(); }

bool open_window(bool fullscreen) {
    const std::string script = ps2::os::env("PS2_PAD_SCRIPT"), pad_log = ps2::os::env("PS2_PAD_LOG"), no_beep = ps2::os::env("PS2_NO_BEEP");
    g_beep_on = no_beep.empty() || no_beep == "0";
    g_pad_log = !pad_log.empty() && pad_log != "0";
    if (!script.empty()) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    // button layouts for pads SDL does not know by itself: a gamecontrollerdb.txt next to ps2run (SDL_GAMECONTROLLERCONFIG_FILE
    // and SDL_GAMECONTROLLERCONFIG work too)
    if (char *base = SDL_GetBasePath()) {
        const std::string db = std::string(base) + "gamecontrollerdb.txt";
        SDL_free(base);
        if (SDL_RWops *file = SDL_RWFromFile(db.c_str(), "rb")) std::fprintf(stderr, "[pad] %d button layouts from %s\n", SDL_GameControllerAddMappingsFromRW(file, 1), db.c_str());
    }
    if (!script.empty()) load_pad_script(script);
    g_keys_path = ps2::os::env("PS2_KEYS");
    if (g_keys_path.empty()) g_keys_path = ps2::quick_folder("keys.ini");  // next to the memory card, like the quick saves
    if (const std::string word = ps2::os::env("PS2_LOOK"); !word.empty()) {
        for (int i = 0; i < keymap::kLookCount; ++i)
            if (word == keymap::look_word(i) || word == std::to_string(i)) g_look = i, g_look_forced = true;
        if (!g_look_forced) std::fprintf(stderr, "[video] PS2_LOOK=%s is not crisp, crisp_edges, soft, soft_edges, sharp, sharp_edges or 0 to 5, ignored\n", word.c_str());
    }
    load_keys();
    {
        std::lock_guard<std::mutex> lk(g_ach_mutex);
        g_achievements.load(ps2::quick_folder("achievements.json"));
    }
    // The inner resolution: PS2_SCALE when it is set, else the 'scale' line of keys.ini (the quick menu writes it), else 1. It is read
    // when the graphics start (run_window, after this), so setting it here is in time.
    g_scale_locked = !ps2::os::env("PS2_SCALE").empty();
    if (!g_scale_locked && g_keys.scale >= 1 && g_keys.scale <= 64) {
        ps2::os::set_env("PS2_SCALE", std::to_string(g_keys.scale));
        std::fprintf(stderr, "[video] resolution %dx from %s (scale = %d)\n", g_keys.scale, g_keys_path.c_str(), g_keys.scale);
    }
    // The picture pace (ps2/pace.hpp): the 'pace' line of keys.ini, unless PS2_PACE is set. The runtime reads it at boot, after this.
    if (!g_keys.pace.empty() && ps2::os::env("PS2_PACE").empty()) {
        ps2::os::set_env("PS2_PACE", g_keys.pace);
        ps2::os::set_env("PS2_PACE_FROM", g_keys_path);
    }
    g_scale_now = std::clamp(std::atoi(ps2::os::env("PS2_SCALE").c_str()), 1, 64);
    g_scale_next = g_scale_locked ? g_scale_now : (g_keys.scale >= 1 ? g_keys.scale : g_scale_now);
    // the 2D art filter the same way: PS2_SHARP_2D, else the 'sharp_2d' line of keys.ini, else sharp (gpu.cpp reads it at its start)
    g_sharp_locked = !ps2::os::env("PS2_SHARP_2D").empty();
    if (!g_sharp_locked && g_keys.sharp_2d >= 0) ps2::os::set_env("PS2_SHARP_2D", std::to_string(g_keys.sharp_2d));
    g_sharp_now = ps2::os::env("PS2_SHARP_2D").empty() ? 1 : std::clamp(std::atoi(ps2::os::env("PS2_SHARP_2D").c_str()), 0, 2);
    g_sharp_next = g_sharp_locked ? g_sharp_now : (g_keys.sharp_2d >= 0 ? g_keys.sharp_2d : g_sharp_now);
    // Graphics chip (PS2_GPU_ADAPTER), graphics API (PS2_GPU_BACKEND), and async GPU submit thread (PS2_GPU_SUBMIT_THREAD)
    g_gpu_locked = !ps2::os::env("PS2_GPU_ADAPTER").empty() || !ps2::os::env("PS2_GPU_LOW_POWER").empty();
    if (!g_gpu_locked && !g_keys.gpu.empty() && g_keys.gpu != "auto") {
        ps2::os::set_env("PS2_GPU_ADAPTER", g_keys.gpu);
        std::fprintf(stderr, "[video] graphics chip \"%s\" from %s (gpu = %s)\n", g_keys.gpu.c_str(), g_keys_path.c_str(), g_keys.gpu.c_str());
    }
    g_gpu_now = !ps2::os::env("PS2_GPU_ADAPTER").empty() ? ps2::os::env("PS2_GPU_ADAPTER") : (!ps2::os::env("PS2_GPU_LOW_POWER").empty() ? "low_power" : "auto");
    g_gpu_next = g_gpu_locked ? g_gpu_now : (!g_keys.gpu.empty() ? g_keys.gpu : g_gpu_now);
    g_gpu_api_locked = !ps2::os::env("PS2_GPU_BACKEND").empty();
    if (!g_gpu_api_locked && !g_keys.gpu_api.empty()) {
        ps2::os::set_env("PS2_GPU_BACKEND", g_keys.gpu_api);
        std::fprintf(stderr, "[video] graphics API %s from %s (gpu_api = %s)\n", g_keys.gpu_api.c_str(), g_keys_path.c_str(), g_keys.gpu_api.c_str());
    }
    {
        std::string api = ps2::os::env("PS2_GPU_BACKEND");
        for (char &c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_gpu_api_now = api == "vulkan" ? "vulkan" : "dx12";
        g_gpu_api_next = g_gpu_api_locked ? g_gpu_api_now : (!g_keys.gpu_api.empty() ? g_keys.gpu_api : g_gpu_api_now);
    }
    if (ps2::os::env("PS2_GPU_SUBMIT_THREAD").empty() && g_keys.gpu_submit_thread >= 0) {
        ps2::os::set_env("PS2_GPU_SUBMIT_THREAD", g_keys.gpu_submit_thread ? "1" : "0");
    }
#ifdef PS2_GPU
    g_gpu_submit_on = ps2::gs::gpu::submit_thread_enabled();
    if (g_keys.gpu_submit_thread >= 0) {
        g_gpu_submit_on = g_keys.gpu_submit_thread != 0;
        ps2::gs::gpu::set_submit_thread_enabled(g_gpu_submit_on);
    }
#endif
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
#ifdef __APPLE__
    if (!std::getenv("PS2_SDL_RENDERER")) flags |= SDL_WINDOW_METAL;
#endif
    g_game_title = ps2::os::env("PS2_WINDOW_TITLE");
    const std::string initial_title = g_game_title.empty() ? g_speed_title : g_game_title + "   |   " + g_speed_title;
    g_window = SDL_CreateWindow(initial_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ps2::g_options.widescreen ? 1280 : 960, 720, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    g_display_hz = query_display_hz();
    apply_video_params();
#ifdef _WIN32
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(g_window, &wm_info) && wm_info.subsystem == SDL_SYSWM_WINDOWS) {
        ShowWindow(static_cast<HWND>(wm_info.info.win.window), SW_SHOWNORMAL);
        SetForegroundWindow(static_cast<HWND>(wm_info.info.win.window));
    }
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    // PS2_AUDIO_STRETCH=0 plays the sound as it comes (silence where it runs short); PS2_AUDIO_STRETCH_MAX=<ratio> changes the limit
    static ps2sound::PipeConfig config;
    const std::string stretch = ps2::os::env("PS2_AUDIO_STRETCH"), stretch_max = ps2::os::env("PS2_AUDIO_STRETCH_MAX");
    if (!stretch.empty()) g_audio_stretch = (stretch != "0");
    config.stretch = g_audio_stretch;
    if (const double limit = std::atof(stretch_max.c_str()); limit >= 1.5 && limit <= 20.0) config.max_ratio = limit;
    static ps2sound::SoundPipe pipe(config);
    g_pipe = &pipe;
    apply_audio_settings();
    if (reopen_audio_device(g_audio_device)) {
        // SDL converts the 48000 Hz stereo queue to whatever the device itself runs at
        SDL_AudioSpec device{};
        char *name = nullptr;
        if (SDL_GetDefaultAudioInfo(&name, &device, 0) == 0)
            std::fprintf(stderr, "[audio] sound device %s (%s): %d Hz, %d channels; the game's sound is 48000 Hz stereo\n", name ? name : "?",
                         SDL_GetCurrentAudioDriver(), device.freq, device.channels);
        SDL_free(name);
        const std::string dump = ps2::os::env("PS2_AUDIO_DUMP");
        if (!dump.empty() && g_dump.open(dump)) {
            g_pipe->device_tap.on = true;
            std::fprintf(stderr, "[audio] writing the sound that goes to the device to %s\n", dump.c_str());
        }
        const std::string heard = ps2::os::env("PS2_AUDIO_HEARD");
        if (!heard.empty() && g_heard.open(heard)) {
            g_pipe->heard_tap.on = true;
            std::fprintf(stderr, "[audio] writing what the speaker plays (silence included) to %s\n", heard.c_str());
        }
        std::fprintf(stderr, "[audio] %s\n", g_audio_stretch ? "when the game runs slower than real time its sound is played slower, with the same pitch (PS2_AUDIO_STRETCH=0 switches that off)"
                                                             : "stretching is off: the sound is played as it comes, with silence where the game was too slow");
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    SDL_ShowCursor(SDL_DISABLE);  // no pointer over the game (the title bar and the window's edge still show it)
    g_notice_text = kStartNotice;
    std::fprintf(stderr, "[menu] the quick menu: F1%s on the keyboard, hold Select then press Start (or Guide) on a controller\n",
                 g_keys.inputs[keymap::Menu].empty() ? "" : (" or " + keymap::input_name(g_keys.inputs[keymap::Menu][0])).c_str());
    g_notice_until = SDL_GetTicks() + 30000;
    refresh_title();
    apply_mouse_mode();
    return true;
}

#ifdef __linux__
// A window run whose game stops (no new frame) used to stay silent: the owner closed the window, nothing answered, the compositor's
// "not responding" window killed the process, and the log ended without a word about where the threads were. This watches the frame
// counter; after kWatchdogSeconds without a new frame it writes one line and the stack of every thread (each thread is sent SIGUSR2 and
// writes its own, which is what the unwinder needs), once per stall. Nothing here touches the game.
namespace {
constexpr int kWatchdogSeconds = 10;
std::atomic<int> g_stack_done{0};

void write_number(long long value) {
    char digits[24];
    int n = 0;
    unsigned long long v = value < 0 ? 0ull : static_cast<unsigned long long>(value);
    do digits[n++] = static_cast<char>('0' + v % 10);
    while ((v /= 10) && n < 23);
    char text[24];
    for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
    ssize_t ignored = write(2, text, static_cast<std::size_t>(n));
    (void)ignored;
}

void on_stack_request(int) {
    const int saved = errno;
    char name[32] = {};
    prctl(PR_GET_NAME, name, 0, 0, 0);
    ssize_t ignored = write(2, "[watchdog] thread ", 18);
    write_number(syscall(SYS_gettid));
    ignored = write(2, " (", 2);
    ignored = write(2, name, std::strlen(name));
    ignored = write(2, "):\n", 3);
    (void)ignored;
#ifdef __GLIBC__
    void *frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
#endif
    g_stack_done.fetch_add(1);
    errno = saved;
}

void report_threads(std::uint64_t frame, int stalled_seconds) {
    std::fprintf(stderr, "[watchdog] no new frame for %d s (stuck at frame %llu); the stack of every thread, as addresses in ps2run (addr2line -f -C -e ps2run <address>):\n", stalled_seconds,
                 static_cast<unsigned long long>(frame));
    {
        const ps2::hitch::PresentStage &stage = ps2::hitch::present_stage();
        const char *where = stage.name.load(std::memory_order_acquire);
        if (where)
            std::fprintf(stderr, "[watchdog] the window thread has been in \"%s\" for %.1f s\n", where,
                         static_cast<double>(ps2::hitch::now_ns() - stage.since_ns.load(std::memory_order_relaxed)) / 1e9);
        else
            std::fprintf(stderr, "[watchdog] the window thread is not in the hand-over of a picture to the screen\n");
        const char *loop = g_window_stage.load(std::memory_order_acquire);
        if (loop)
            std::fprintf(stderr, "[watchdog] the window thread's loop has been in \"%s\" for %.1f s\n", loop,
                         static_cast<double>(ps2::hitch::now_ns() - g_window_stage_since_ns.load(std::memory_order_relaxed)) / 1e9);
        if (g_card.state.load() == 1) std::fprintf(stderr, "[watchdog] the key card is open (its box runs on a thread of its own)\n");
    }
    std::fflush(stderr);
    const pid_t self = getpid();
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        const long tid = std::strtol(entry.path().filename().string().c_str(), nullptr, 10);
        if (tid <= 0 || tid == syscall(SYS_gettid)) continue;
        const int before = g_stack_done.load();
        if (syscall(SYS_tgkill, self, tid, SIGUSR2) != 0) continue;
        for (int waited = 0; g_stack_done.load() == before && waited < 500; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (g_stack_done.load() == before) std::fprintf(stderr, "[watchdog] thread %ld did not answer in 1 s (blocked inside the kernel)\n", tid);
    }
}

void watchdog() {
    ps2::crash::enter_thread(ps2::crash::Thread::Watchdog);
    std::signal(SIGUSR2, on_stack_request);
#ifdef __GLIBC__
    void *warm[2];
    backtrace(warm, 2);  // loads the unwinder now, not inside the handler
#endif
    std::uint64_t seen = ps2::frames_run();
    auto since = std::chrono::steady_clock::now();
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t frame = ps2::frames_run();
        const auto now = std::chrono::steady_clock::now();
        const int stalled = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - since).count());
        if (g_menu_hold.load()) since = now;  // the game waits for the quick menu: no stall
        if (frame != seen) {
            if (reported) std::fprintf(stderr, "[watchdog] the game runs again at frame %llu after %d s\n", static_cast<unsigned long long>(frame), stalled);
            seen = frame;
            since = now;
            reported = false;
        } else if (!reported && stalled >= kWatchdogSeconds) {
            reported = true;
            report_threads(frame, stalled);
        }
    }
}
}  // namespace
#endif

int run_window(const std::function<int()> &emulate) {
    ps2::init_graphics();
#ifdef PS2_GPU
    if (!ps2::gs::gpu::active_backend().empty()) {
        g_gpu_api_now = ps2::gs::gpu::active_backend();
        if (!g_gpu_api_locked && g_keys.gpu_api.empty()) g_gpu_api_next = g_gpu_api_now;
    }
    g_scale_now = static_cast<int>(ps2::gs::gpu::active_scale());
    if (!g_scale_locked && g_keys.scale <= 0) g_scale_next = g_scale_now;
    g_gpu_submit_on = ps2::gs::gpu::submit_thread_enabled();
#endif
    g_gpu_present = attach_gpu();
#ifdef PS2_GPU
    if (g_gpu_present) {
        if (!g_keys.present_mode.empty()) ps2::gs::gpu::set_present_mode(g_keys.present_mode);
        g_present_mode = ps2::gs::gpu::active_present_mode();
    }
#endif
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        // The game runs on this thread, so while it waits for a live translation (15 to 35 s) it must still take the window's messages, or
        // Windows marks the window "not responding" and ends the program when it is clicked (ee_live.cpp, while_pumping).
        ps2::g_options.keep_alive = [] { poll(); };
        const int result = emulate();
        g_achievements.flush();
        screenshot::wait_pending();
        finish_audio();
#ifdef __APPLE__
        destroy_metal_view();
#endif
        return result;
    }
    ps2::g_options.on_vblank = [] {
        g_emu_cpu_ms.store(ps2::hitch::thread_cpu_ms(), std::memory_order_relaxed);
        poll_achievements_emulation();
        const auto t0 = WaitLog::Clock::now();
        ps2::post_present();
        const auto t1 = WaitLog::Clock::now();
        pace();
        hold_for_menu();
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: a frame of the emulation thread that took long, and what it waited for (docs/hitches.md)
            static auto last = WaitLog::Clock::now();
            static double last_cpu = ps2::hitch::thread_cpu_ms();
            const auto now = WaitLog::Clock::now();
            const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count(), cpu = ps2::hitch::thread_cpu_ms();
            if (frame_ms > ps2::hitch::threshold_ms())
                std::fprintf(stderr, "[hitch] frame %llu took %.1f ms: waited for the window thread %.1f ms, for the sound %.1f ms; the emulation thread itself used %.1f ms of CPU; sound queued %.0f ms\n",
                             static_cast<unsigned long long>(ps2::frames_run()), frame_ms, std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), cpu - last_cpu, queued_ms());
            last = now;
            last_cpu = cpu;
        }
        if (g_frame_log.file)
            g_frame_log.frame(std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), queued_ms());
        if (g_waits.on) {
            g_waits.post += std::chrono::duration<double, std::milli>(t1 - t0).count();
            g_waits.sound += WaitLog::ms(t1);
            g_waits.emulation_frame();
        }
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
#ifdef __linux__
    if (!std::getenv("PS2_NO_WATCHDOG")) std::thread(watchdog).detach();
#endif
    std::atomic<bool> done{false};
    int result = 0;
    ps2::host::BigStackThread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    while (!done.load()) {
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: the window's own event handling
            const auto p0 = WaitLog::Clock::now();
            poll();
            const double p_ms = WaitLog::ms(p0);
            if (p_ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] window event polling took %.1f ms (around frame %llu)\n", p_ms, static_cast<unsigned long long>(ps2::frames_run()));
        } else {
            poll();
        }
        window_stage("drawing the pictures of the emulation (render_pending)");
        if (!g_waits.on) {
            if (ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle) {
                window_stage("waiting for the emulation to post work (wait_render_work)");
                ps2::wait_render_work(4);
            }
            continue;
        }
        const auto t0 = WaitLog::Clock::now();
        const bool idle = ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle;
        const auto t1 = WaitLog::Clock::now();
        if (idle) {
            window_stage("waiting for the emulation to post work (wait_render_work)");
            ps2::wait_render_work(4);
        }
        g_waits.working.store(g_waits.working.load() + std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (idle) g_waits.idle.store(g_waits.idle.load() + WaitLog::ms(t1));
    }
    emulation.join();
    g_achievements.flush();
    screenshot::wait_pending();
    ps2::flush_memory_card();
    finish_audio();
#ifdef __APPLE__
    destroy_metal_view();
#endif
    if (g_card.state.load() == 0) SDL_Quit();  // a key card still open is on a thread inside SDL: the program ends without taking SDL down under it
    if (ps2::g_options.on_exit) ps2::g_options.on_exit();  // the host's log copy hands over the last lines before the process ends
    std::fflush(nullptr);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    std::_Exit(result);
}
