#pragma once

// The keyboard and mouse layout of a window run (docs/keys.md): which key or mouse button works which button of the console's
// controller, the key file the player can edit (keys.ini), and the key card (the quick menu's Keys entry) made from what is in force.

#include <string>
#include <vector>

namespace keymap {

// What a key can do. The first 16 are the pad's buttons in the order of the controller layout in window.cpp.
enum Action {
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LeftUp, LeftDown, LeftLeft, LeftRight,      // left stick, pushed all the way
    RightUp, RightDown, RightLeft, RightRight,  // right stick, pushed all the way
    Cross, Circle, Square, Triangle,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    Roll,  // while held, the direction keys of the left stick push the right stick as well (God of War's evade roll)
    Rage,  // L3 and R3 pressed together, which one key can do and two keys can hardly
    Help,  // a key for the key card (none by default; the quick menu's Keys entry shows it)
    Menu,  // another key for the quick menu (none by default; F1 always opens it), host/quickmenu.cpp
    ActionCount
};

// An input is a number: a keyboard scancode (SDL's, below 512: the key by its place on the keyboard), or one of these.
constexpr int kMouseLeft = 1000, kMouseMiddle = 1001, kMouseRight = 1002, kMouseX1 = 1003, kMouseX2 = 1004;
constexpr int kWheelUp = 1010, kWheelDown = 1011;

struct Config {
    std::vector<int> inputs[ActionCount];
    int mouse_stick = 0;       // 0 the mouse moves no stick, 1 it pushes the left stick, 2 the right stick
    double mouse_speed = 1.0;  // 0.1 to 10
    int mouse_invert = 0;      // 0 none, 1 left-right, 2 up-down, 3 both
    double mouse_deadzone = 0; // 0 to 0.9: how far the stick jumps out as soon as the mouse moves
    int look = -1;             // the picture look (F2) written in keys.ini, -1 when the file has no 'look' line
    int scale = 0;             // the inner resolution (1 to gpu_max_scale) used at the next start, 0 when the file has no 'scale' line
    int sharp_2d = -1;         // how upscaled 2D art is filtered (0 soft, 1 sharp, 2 sharpest) at the next start, -1 when the file has no line
    int sharp_3d = 0;          // 1 = sharp-bilinear 3D texture filter (PS2_SHARP_3D=1), 0 = classic PS2 bilinear
    std::string pace;          // the picture pace ('30', '60', 'off', ...; ps2/pace.hpp) used at the next start, "" when the file has no 'pace' line
    std::string gpu;           // the graphics chip ('auto', 'low_power', or a chip name) used at the next start, "" when the file has no 'gpu' line
    std::string gpu_api;       // the graphics API ('vulkan' or 'dx12') used at the next start, "" when the file has no 'gpu_api' line
    int gpu_submit_thread = -1; // async GPU submit thread (1 on, 0 off), -1 when the file has no 'gpu_submit_thread' line
    // Live Graphics / Display controls
    int aspect = 0;            // 0 = 4:3, 1 = 16:9, 2 = stretch
    bool aspect_explicit = false; // true when keys.ini explicitly set 'aspect'
    int fullscreen = 0;        // 0 = windowed, 1 = fullscreen
    int title_perf = 0;        // 0 = clean titlebar, 1 = live FPS/CPU/audio telemetry
    int perf_hud = 0;          // 0 = off, 1 = compact fullscreen corner FPS/frame-time HUD (toggled with F10)
    int turbo_speed = 3;       // Turbo / Fast-Forward speed multiplier (2, 3, 4, or 0 = Max/Uncapped)
    int brightness = 0;        // -50 .. +50 (percent offset)
    int contrast = 100;        //  50 .. 150 (percent)
    int saturation = 100;      //   0 .. 200 (percent)
    int gamma = 100;           //  50 .. 200 (percent, 100 = 1.00)
    int scanlines = 0;         //   0 .. 100 (percent, 0 = off)
    int deinterlace = 1;       // 1 = on, 0 = off
    std::string present_mode;  // "", "mailbox", "fifo", "immediate", "fiforelaxed"
    std::string fps_cap = "60"; // "30", "60", "120", "144", "display", "unlimited"
    int fps_speed_sync = 1;    // 1 = on (default), 0 = off
    int toast_theme = 0;       // 0 = tier_tinted, 1 = soul_graveyard, 2 = judgment_ring, 3 = occult_grimoire
    // Live Audio controls
    int master_volume = 100;   // 0 .. 100
    int mute = 0;              // 0 = off, 1 = on
    std::string audio_device;  // "" = system default, else SDL audio device name
    int audio_stretch = -1;    // -1 unset, 0 off, 1 on
    int chime_volume = 100;    // 0 .. 100
    int bgm_volume = 100;      // 0 .. 100 (SPU2 Core 0 music/voices)
    int sfx_volume = 100;      // 0 .. 100 (SPU2 Core 1 sound effects)
    // Phase 2: Save State Slots (0..9), GPU Tile Binning, and Depth-Aware Post-Processing
    int save_slot = 0;               // 0 .. 9
    int slot_picker_on_quickkey = 0; // 0 = instant F5/F8 on active slot, 1 = F5/F8 opens Slot 0-9 Picker
    int gpu_binning = -1;            // -1 auto (on at >=2x), 0 off, 1 on
    int ssao = 0;                    // 0 .. 100
    int bloom = 0;                   // 0 .. 100
    int ssgi = 0;                    // 0 .. 100
    int filmic = 0;                  // 0 off, 1 on
    int vignette = 0;                // 0 .. 100
    int fast_disc = 1;               // 1 = fast SSD disc loading (default), 0 = authentic PS2 DVD speed
    int adaptive_scale = 0;          // 0 = off (default), 1 = auto step down 4x -> 3x under heavy battle GPU load
    int true_widescreen = 1;         // 1 = Hor+ 16:9 3D camera projection & frustum fix when aspect=16:9 (default), 0 = stretch
    int autosave = 1;                // 1 = rolling area-transition autosave (autosave.state, default), 0 = off
    // HD Texture Packs, Smart Runtime Dumper & HD FMV Replacement
    int hd_textures = 1;             // 1 = load enabled packs under mods/textures/ (default), 0 = off
    int hd_fmv = 1;                  // 1 = play matching .mp4 cutscenes under mods/fmv/ (default), 0 = off
    int tex_dump = 0;                // 0 = off (default), 1 = dump static textures to dumps/textures/
    int fmv_dump = 0;                // 0 = off (default), 1 = log & dump MPEG-2 cutscenes to dumps/fmv/
    std::string texture_packs;       // comma-separated pack priority list ("PackA,PackB,!DisabledPack")
};

// The picture looks the F2 key goes through. The number is also the video option bits (ps2/video.hpp): bit 0 smooth edges, bit 1 soft, bit 2 sharp (look 4: cubic enlargement + light sharpening; look 5 = sharp + smooth edges).
constexpr int kLookCount = 6;
constexpr int kDefaultLook = 5;
const char *look_name(int look);           // "Soft + smooth edges", for the title and the key card
const char *look_word(int look);           // the word written in keys.ini
// keys.ini text with its 'look' line set to `look` (other lines untouched; the line is added at the end when missing)
std::string with_look(const std::string &file_text, int look);
// the same for the 'scale' line (the quick menu's resolution, used when the game starts the next time)
std::string with_scale(const std::string &file_text, int scale);
// the same for the 'sharp_2d' line (the quick menu's 2D art filter, used when the game starts the next time)
std::string with_sharp_2d(const std::string &file_text, int sharp);
const char *sharp_2d_word(int sharp);  // "soft", "sharp", "sharpest"
std::string with_gpu(const std::string &file_text, const std::string &gpu);
std::string with_gpu_api(const std::string &file_text, const std::string &api);
std::string with_gpu_submit_thread(const std::string &file_text, bool on);
std::string with_setting(const std::string &file_text, const std::string &key, const std::string &value);

const char *action_name(int action);  // the word used in keys.ini

// The quick menu's "Change keys" page (docs/keys.md): one line for every PS2 controller button, in the order of
// keys.ini (the actions 0 to kChangeableCount - 1; help and menu are left to the file).
constexpr int kChangeableCount = Select + 1;
const char *action_label(int action);                      // "Square", "Left stick Up", "Start"
std::string keys_text(const Config &config, int action);   // "Left Mouse", "Space, Enter, Keypad Enter", "none"
// keys.ini text with the line of `action` set to the keys in `config` (other lines, comments included, untouched; the line is added at the end when missing)
std::string with_inputs(const std::string &file_text, int action, const std::vector<int> &inputs);
// Gives `input` (a key or mouse button) to `action`, which loses the keys it had. A button that already had `input` swaps: it gets the keys
// `action` had. Returns false and a reason in `message` when the input belongs to the program. Otherwise `message` says what happened and
// `changed` holds every action whose keys are different now (the one asked for first).
bool assign_input(Config &config, int action, int input, std::vector<int> &changed, std::string &message);
// The keys of the controller's buttons, roll and rage back to the defaults (the mouse and picture lines are not touched). `changed` as above.
void restore_default_inputs(Config &config, std::vector<int> &changed);
Config defaults();

// Reads the text of a key file on top of `config` (which starts as defaults()). Every problem becomes a line in `problems`; the
// line it is about is ignored, the rest still counts.
void parse(const std::string &text, Config &config, std::vector<std::string> &problems);

std::string input_name(int input);              // "Left Shift", "Left Mouse"
std::string default_file_text();                // keys.ini as written on the first start
std::string card_text(const Config &config);    // the lines of the key card
// the program's own keys cannot be given to the game: F1, F2, F5, F8, F9, F11
bool is_reserved(int input);

}  // namespace keymap
