#include "launcher_data.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#endif

namespace launcher {
namespace {

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_bool(const std::string &v, bool fallback) {
    const std::string w = to_lower(trim(v));
    if (w == "on" || w == "true" || w == "1" || w == "yes") return true;
    if (w == "off" || w == "false" || w == "0" || w == "no") return false;
    return fallback;
}

std::string read_text_file(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text_atomic(const std::filesystem::path &p, const std::string &content) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return false;
    }
#ifdef _WIN32
    if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#endif
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

std::filesystem::path find_disc(const std::filesystem::path &game_dir, const char *folder_name, const char *iso_name) {
    std::error_code ec;
    const std::filesystem::path folder = game_dir / folder_name;
    if (std::filesystem::is_regular_file(folder / "disc_manifest.tsv", ec)) return folder;
    const std::filesystem::path iso = game_dir / iso_name;
    if (std::filesystem::is_regular_file(iso, ec)) return iso;
    return {};
}

std::string format_file_time(std::filesystem::file_time_type ftime) {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sctp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min);
    return buf;
}

std::string now_file_stamp() {
    const std::time_t tt = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d_%02d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

std::wstring quote_arg(const std::wstring &s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out.push_back(c);
        }
    }
    if (backslashes) out.append(backslashes * 2, L'\\');
    out += L"\"";
    return out;
}

std::string default_keys_ini_template() {
    return
        "# keys.ini: which key or mouse button works which button of the game's controller.\n"
        "# Change a line, save the file, click back into the game window: the new keys are used at once.\n"
        "# F1 opens the quick menu; F5/F8 quick save/load; F6/F7 cycle slots; F11 full screen; F12 screenshot.\n\n"
        "# D-pad\n"
        "dpad_up     = Up\n"
        "dpad_down   = Down\n"
        "dpad_left   = Left\n"
        "dpad_right  = Right\n\n"
        "# left stick (walking)\n"
        "left_up     = W\n"
        "left_down   = S\n"
        "left_left   = A\n"
        "left_right  = D\n\n"
        "# right stick (camera / auxiliary controls)\n"
        "right_up    = I\n"
        "right_down  = K\n"
        "right_left  = J\n"
        "right_right = L\n\n"
        "# face buttons\n"
        "cross       = X\n"
        "circle      = C\n"
        "square      = Z\n"
        "triangle    = V\n\n"
        "# shoulder buttons\n"
        "l1          = E\n"
        "r1          = U\n"
        "l2          = Q\n"
        "r2          = O\n\n"
        "# stick clicks\n"
        "l3          = F\n"
        "r3          = H\n\n"
        "# Start and Select\n"
        "start       = Enter\n"
        "select      = Right Shift\n\n"
        "mouse_stick = off\n"
        "mouse_speed = 1\n"
        "mouse_invert = off\n"
        "mouse_deadzone = 0\n";
}

}  // namespace

const GameProfile &game_profile(GameId id) {
    static const GameProfile kProfiles[4] = {
        {
            GameId::ShadowHearts1,
            "sh1",
            "I",
            "SHADOW HEARTS",
            "1913",
            "Shadow Hearts",
            "Shadow Hearts",
            "1913 China & Europe",
            "1913 China & Europe  •  60+ FPS Native Port  •  True 16:9 Hor+  •  75 Trophies",
            "SLUS-20347",
            "Disc (SLUS-20347)",
            "",
            "SH1.exe",
            "native-sh1",
            "Shadow Hearts",
            "ShadowHearts",
            "PLAY SHADOW HEARTS",
            "SHADOW HEARTS PROGRESS",
            "Build Shadow Hearts first to awaken the 1913 Judgment Ring.",
            1,
            true,
            true,
        },
        {
            GameId::Covenant,
            "sh2-covenant",
            "II",
            "SHADOW HEARTS:",
            "COVENANT",
            "Shadow Hearts: Covenant",
            "Covenant",
            "1915 Europe & Japan",
            "1915 Europe & Japan  •  60+ FPS Native Port  •  Seamless 2-Disc Auto-Swap  •  110 Trophies",
            "SLUS-21041 / 21044",
            "Disc 1 (SLUS-21041)",
            "Disc 2 (SLUS-21044)",
            "SH2C.exe",
            "native-sh2-covenant",
            "Covenant",
            "ShadowHeartsCovenant",
            "PLAY COVENANT",
            "COVENANT PROGRESS",
            "Build Shadow Hearts: Covenant first to awaken the 1915 Judgment Ring.",
            2,
            true,
            true,
        },
        {
            GameId::FromTheNewWorld,
            "sh3-ftnw",
            "III",
            "SHADOW HEARTS:",
            "FROM THE NEW WORLD",
            "Shadow Hearts: From the New World",
            "From the New World",
            "1929 Americas",
            "1929 New York & The Americas  •  60+ FPS Native Port  •  True 16:9 Hor+  •  75 Trophies",
            "SLUS-21326",
            "Disc (SLUS-21326)",
            "",
            "SH3.exe",
            "native-sh3-ftnw",
            "From the New World",
            "ShadowHeartsNewWorld",
            "PLAY NEW WORLD",
            "NEW WORLD PROGRESS",
            "Build Shadow Hearts: From the New World first to open the 1929 Gate.",
            1,
            true,
            true,
        },
        {
            GameId::Koudelka,
            "koudelka",
            "0",
            "PREQUEL  •  1898",
            "KOUDELKA",
            "Koudelka",
            "Koudelka",
            "1898 Nemeton Monastery, Wales",
            "1898 Nemeton Monastery  •  PS1 Gothic Survival-Horror RPG  •  4-Disc Auto-Swap  •  50 Trophies",
            "SLUS-01051..01102",
            "4-Disc Library (SLUS-01051..02)",
            "",
            "Koudelka.exe",
            "native-koudelka",
            "Koudelka",
            "Koudelka",
            "PLAY KOUDELKA",
            "KOUDELKA PROGRESS",
            "Koudelka 4-disc virtual library is staged — click Build / Setup to compile Koudelka.exe.",
            4,
            true,
            false,
        },
    };
    const int idx = static_cast<int>(id);
    return kProfiles[(idx >= 0 && idx < 4) ? idx : 1];
}

GameId game_id_from_slug(const std::string &slug) {
    const std::string s = to_lower(trim(slug));
    if (s == "sh1" || s == "shadow-hearts" || s == "shadowhearts") return GameId::ShadowHearts1;
    if (s == "sh3" || s == "sh3-ftnw" || s == "ftnw" || s == "from-the-new-world") return GameId::FromTheNewWorld;
    if (s == "koudelka" || s == "ps1" || s == "prequel") return GameId::Koudelka;
    return GameId::Covenant;
}

std::filesystem::path default_screenshots_folder(GameId game) {
    std::filesystem::path base;
#ifdef _WIN32
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &wpath)) && wpath) {
        base = std::filesystem::path(wpath);
        CoTaskMemFree(wpath);
    } else if (const char *prof = std::getenv("USERPROFILE"); prof && *prof) {
        base = std::filesystem::path(prof) / "Pictures";
    }
#else
    if (const char *home = std::getenv("HOME"); home && *home) base = std::filesystem::path(home) / "Pictures";
#endif
    if (base.empty()) base = std::filesystem::current_path();
    return base / "Shadow Hearts Nocturne" / game_profile(game).screenshot_subdir;
}

std::string sanitize_filename(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    bool prev_space = false;
    for (unsigned char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || c < 32) continue;
        if (c == ' ') {
            if (!out.empty() && !prev_space) out.push_back('_');
            prev_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            prev_space = false;
        }
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    return out.empty() ? "Unlocked" : out;
}

LauncherPaths resolve_paths(const std::filesystem::path &root_dir, GameId game) {
    LauncherPaths p;
    std::error_code ec;
    const GameProfile &prof = game_profile(game);
    p.root_dir = std::filesystem::absolute(root_dir, ec).lexically_normal();
    if (p.root_dir.empty()) p.root_dir = root_dir;

    // Resolve scripts/Setup.cmd root: prefer root_dir, else fall back to source_repo in launcher.ini
    // so standalone custom installs (e.g. D:\Games\Shadow Hearts Nocturne\) still run Tools & Updates seamlessly.
    p.scripts_root = p.root_dir;
    if (!std::filesystem::is_regular_file(p.root_dir / "Shadow Hearts Nocturne Setup.cmd", ec) &&
        !std::filesystem::is_regular_file(p.root_dir / "scripts" / "nocturne_update.py", ec)) {
        const GlobalLauncherConfig gcfg = load_global_config(p.root_dir);
        if (!gcfg.source_repo.empty()) {
            const std::filesystem::path sr = std::filesystem::path(gcfg.source_repo).lexically_normal();
            if (std::filesystem::is_directory(sr, ec) &&
                (std::filesystem::is_regular_file(sr / "Shadow Hearts Nocturne Setup.cmd", ec) ||
                 std::filesystem::is_regular_file(sr / "scripts" / "nocturne_update.py", ec))) {
                p.scripts_root = sr;
            }
        }
    }

    p.game_dir = p.root_dir / "games" / prof.slug;
    p.game_exe = p.game_dir / prof.exe_name;
    if (!std::filesystem::is_regular_file(p.game_exe, ec)) {
        const std::filesystem::path fallback = p.scripts_root / "build" / prof.build_dir_name / "ps2run.exe";
        if (std::filesystem::is_regular_file(fallback, ec)) p.game_exe = fallback;
    }

    p.keys_ini = p.game_dir / "keys.ini";
    p.achievements_json = p.game_dir / "achievements.json";
    p.playtime_json = p.game_dir / "playtime.json";
    p.memcard_path = p.game_dir / ((game == GameId::Koudelka) ? "memcard.mcr" : "memcard.ps2");
    p.states_dir = p.game_dir / "states";
    p.logs_dir = p.game_dir / "logs";
    p.backups_dir = p.game_dir / "backups";
    p.screenshots_dir = default_screenshots_folder(game);
    p.texture_packs_dir = p.game_dir / "texture_packs";
    p.video_packs_dir = p.game_dir / "video_packs";
    p.mods_dir = p.game_dir / "mods";
    p.mods_textures_dir = p.game_dir / "mods" / "textures";
    p.mods_fmv_dir = p.game_dir / "mods" / "fmv";
    p.dumps_textures_dir = p.game_dir / "dumps" / "textures";
    p.dumps_fmv_dir = p.game_dir / "dumps" / "fmv";
    p.upscale_script = p.scripts_root / "scripts" / "upscale_textures.py";
    p.upscale_fmv_script = p.scripts_root / "scripts" / "upscale_fmvs.py";
    p.update_script = p.scripts_root / "scripts" / "nocturne_update.py";
    p.build_cmd = p.scripts_root / "Shadow Hearts Nocturne Setup.cmd";
    if (game == GameId::Covenant) {
        p.extract_pkb_script = p.scripts_root / "scripts" / "sh2_extract_pkb.py";
        p.precompile_vu1_script = p.scripts_root / "scripts" / "sh2_precompile_vu1.py";
    } else if (game == GameId::ShadowHearts1) {
        p.extract_pkb_script = p.scripts_root / "scripts" / "sh1_extract_pkb.py";
        p.precompile_vu1_script = p.scripts_root / "scripts" / "sh1_precompile_vu1.py";
    } else if (game == GameId::FromTheNewWorld) {
        p.extract_pkb_script = p.scripts_root / "scripts" / "sh3_extract_pkb.py";
        p.precompile_vu1_script = p.scripts_root / "scripts" / "sh3_precompile_vu1.py";
    }
    if (prof.disc_count == 2) {
        p.disc1_path = find_disc(p.game_dir, "disc1_files", "disc.iso");
        p.disc2_path = find_disc(p.game_dir, "disc2_files", "disc2.iso");
    } else if (prof.disc_count == 4) {
        p.disc1_path = find_disc(p.game_dir, "disc_files", "disc1.bin");
    } else {
        p.disc1_path = find_disc(p.game_dir, "disc_files", "disc.iso");
    }
    for (const char *ext : {"cover.png", "cover.jpg", "cover.jpeg", "cover.webp"}) {
        const std::filesystem::path cand = p.game_dir / ext;
        if (std::filesystem::is_regular_file(cand, ec)) {
            p.cover_image = cand;
            break;
        }
    }
    return p;
}

GlobalLauncherConfig load_global_config(const std::filesystem::path &root_dir) {
    GlobalLauncherConfig cfg;
    bool channel_set = false;
    const std::string text = read_text_file(root_dir / "launcher.ini");
    if (!text.empty()) {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = to_lower(trim(line.substr(0, eq)));
            const std::string v = trim(line.substr(eq + 1));
            if (k == "chimes" || k == "launcher_chimes") cfg.chimes = parse_bool(v, cfg.chimes);
            else if (k == "auto_enter_last_game") cfg.auto_enter_last_game = parse_bool(v, cfg.auto_enter_last_game);
            else if (k == "last_game") cfg.last_game = game_id_from_slug(v);
            else if (k == "update_channel") {
                const std::string low = to_lower(v);
                if (low == "main" || low == "nightly") {
                    cfg.update_channel = low;
                    channel_set = true;
                }
            } else if (k == "source_repo" && !v.empty()) {
                cfg.source_repo = v;
            } else if (k == "coop_player_name" && !v.empty()) {
                cfg.coop_player_name = v;
            } else if (k == "coop_relay_url") {
                cfg.coop_relay_url = v;
            } else if (k == "coop_campaign" && !v.empty()) {
                cfg.coop_campaign = v;
            }
        }
    }
    const std::filesystem::path probe_root = !cfg.source_repo.empty() ? std::filesystem::path(cfg.source_repo) : root_dir;
    // Fall back to install_config.json or .git/HEAD so nightly installs stay on nightly by default
    if (!channel_set) {
        std::string ic = read_text_file(root_dir / "install_config.json");
        if (ic.empty() && probe_root != root_dir) ic = read_text_file(probe_root / "install_config.json");
        if (ic.find("\"update_channel\"") != std::string::npos && ic.find("\"nightly\"") != std::string::npos) {
            cfg.update_channel = "nightly";
            channel_set = true;
        }
    }
    if (!channel_set) {
        std::string head = read_text_file(root_dir / ".git" / "HEAD");
        if (head.empty() && probe_root != root_dir) head = read_text_file(probe_root / ".git" / "HEAD");
        if (head.find("refs/heads/nightly") != std::string::npos) {
            cfg.update_channel = "nightly";
        }
    }
    return cfg;
}

bool save_global_config(const std::filesystem::path &root_dir, const GlobalLauncherConfig &cfg) {
    const std::filesystem::path path = root_dir / "launcher.ini";
    std::string text = read_text_file(path);
    if (text.empty()) {
        text = "# Shadow Hearts: Nocturne — Trilogy Launcher Configuration\n";
    }
    text = upsert_ini_setting(text, "chimes", cfg.chimes ? "on" : "off");
    text = upsert_ini_setting(text, "auto_enter_last_game", cfg.auto_enter_last_game ? "on" : "off");
    text = upsert_ini_setting(text, "last_game", game_profile(cfg.last_game).slug);
    text = upsert_ini_setting(text, "update_channel", (cfg.update_channel == "nightly") ? "nightly" : "main");
    if (!cfg.source_repo.empty()) {
        text = upsert_ini_setting(text, "source_repo", cfg.source_repo);
    }
    if (!cfg.coop_player_name.empty()) {
        text = upsert_ini_setting(text, "coop_player_name", cfg.coop_player_name);
    }
    text = upsert_ini_setting(text, "coop_relay_url", cfg.coop_relay_url);
    if (!cfg.coop_campaign.empty()) {
        text = upsert_ini_setting(text, "coop_campaign", cfg.coop_campaign);
    }
    return write_text_atomic(path, text);
}

UpdateCheckResult parse_update_check_json(const std::string &json) {
    UpdateCheckResult r;
    r.checked = true;
    r.checking = false;
    auto find_bool = [&](const char *key, bool fallback) -> bool {
        const std::string needle = std::string("\"") + key + "\"";
        const std::size_t pos = json.find(needle);
        if (pos == std::string::npos) return fallback;
        const std::size_t col = json.find(':', pos + needle.size());
        if (col == std::string::npos) return fallback;
        std::size_t i = col + 1;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (json.compare(i, 4, "true") == 0) return true;
        if (json.compare(i, 5, "false") == 0) return false;
        return fallback;
    };
    auto find_str = [&](const char *key) -> std::string {
        const std::string needle = std::string("\"") + key + "\"";
        const std::size_t pos = json.find(needle);
        if (pos == std::string::npos) return {};
        const std::size_t col = json.find(':', pos + needle.size());
        if (col == std::string::npos) return {};
        const std::size_t q1 = json.find('"', col + 1);
        if (q1 == std::string::npos) return {};
        std::string out;
        for (std::size_t i = q1 + 1; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) {
                const char nx = json[++i];
                if (nx == 'n') out.push_back(' ');
                else out.push_back(nx);
            } else if (json[i] == '"') {
                break;
            } else {
                out.push_back(json[i]);
            }
        }
        return out;
    };
    r.ok = find_bool("ok", false);
    r.update_available = find_bool("update_available", false);
    const std::string ch = find_str("channel");
    if (!ch.empty()) r.channel = ch;
    r.local_short = find_str("local_short");
    r.local_date = find_str("local_date");
    r.remote_short = find_str("remote_short");
    r.remote_date = find_str("remote_date");
    r.remote_subject = find_str("remote_subject");
    r.error = find_str("error");
    if (!r.ok && r.error.empty()) r.error = "Could not parse updater response.";
    return r;
}

std::string upsert_ini_setting(const std::string &text, const std::string &key, const std::string &value) {
    const std::string lower_key = to_lower(key);
    const std::string replacement = key + " = " + value;
    std::string out;
    out.reserve(text.size() + replacement.size() + 8);
    bool replaced = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const bool has_nl = (nl != std::string::npos);
        std::string line = has_nl ? text.substr(pos, nl - pos) : text.substr(pos);
        const bool has_cr = !line.empty() && line.back() == '\r';
        if (has_cr) line.pop_back();

        const std::string t = trim(line);
        bool match = false;
        if (!t.empty() && t[0] != '#' && t[0] != ';') {
            const std::size_t eq = t.find('=');
            if (eq != std::string::npos && to_lower(trim(t.substr(0, eq))) == lower_key) match = true;
        }
        if (match) {
            if (!replaced) {
                out += replacement;
                if (has_cr) out.push_back('\r');
                if (has_nl) out.push_back('\n');
                replaced = true;
            }
        } else {
            out += line;
            if (has_cr) out.push_back('\r');
            if (has_nl) out.push_back('\n');
        }
        if (!has_nl) break;
        pos = nl + 1;
    }
    if (!replaced) {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        out += replacement;
        out.push_back('\n');
    }
    return out;
}

Settings load_settings(const std::filesystem::path &keys_ini_path) {
    Settings s;
    const std::string text = read_text_file(keys_ini_path);
    if (text.empty()) return s;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = to_lower(trim(t.substr(0, eq)));
        const std::string v = trim(t.substr(eq + 1));
        const std::string vl = to_lower(v);

        if (k == "scale") s.scale = std::clamp(std::atoi(v.c_str()), 1, 64);
        else if (k == "aspect") {
            if (vl == "4:3" || vl == "0") s.aspect = 0;
            else if (vl == "16:9" || vl == "1") s.aspect = 1;
            else if (vl == "stretch" || vl == "2") s.aspect = 2;
            else if (vl == "auto" || vl == "3") s.aspect = 3;
            else if (vl == "16:10" || vl == "4") s.aspect = 4;
            else if (vl == "21:9" || vl == "5") s.aspect = 5;
            else if (vl == "32:9" || vl == "6") s.aspect = 6;
        } else if (k == "fps_cap") s.fps_cap = v.empty() ? "60" : v;
        else if (k == "fps_speed_sync") s.fps_speed_sync = parse_bool(v, true);
        else if (k == "gpu_api") s.gpu_api = (vl == "dx12" || vl == "d3d12") ? "dx12" : "vulkan";
        else if (k == "present_mode") {
            if (vl == "fifo") s.present_mode = "Fifo";
            else if (vl == "immediate") s.present_mode = "Immediate";
            else s.present_mode = "Mailbox";
        } else if (k == "look") s.look = v;
        else if (k == "sharp_2d") {
            if (vl == "soft" || vl == "0") s.sharp_2d = "soft";
            else if (vl == "sharp" || vl == "1") s.sharp_2d = "sharp";
            else s.sharp_2d = "sharpest";
        } else if (k == "sharp_3d") s.sharp_3d = parse_bool(v, false);
        else if (k == "deinterlace") s.deinterlace = parse_bool(v, true);
        else if (k == "gpu_binning") s.gpu_binning = parse_bool(v, true);
        else if (k == "adaptive_scale") s.adaptive_scale = parse_bool(v, false);
        else if (k == "perf_hud") s.perf_hud = parse_bool(v, false);
        else if (k == "scanlines") s.scanlines = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "saturation") s.saturation = std::clamp(std::atoi(v.c_str()), 0, 200);
        else if (k == "ssao") s.ssao = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "bloom") s.bloom = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "ssgi") s.ssgi = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "filmic") s.filmic = parse_bool(v, false);
        else if (k == "vignette") s.vignette = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "master_volume") s.master_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "bgm_volume") s.bgm_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "sfx_volume") s.sfx_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "chime_volume") s.chime_volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "mute") s.mute = parse_bool(v, false);
        else if (k == "audio_stretch") s.audio_stretch = parse_bool(v, true);
        else if (k == "fast_disc") s.fast_disc = parse_bool(v, true);
        else if (k == "true_widescreen") s.true_widescreen = parse_bool(v, true);
        else if (k == "autosave") s.autosave = parse_bool(v, true);
        else if (k == "turbo_speed") {
            if (vl == "max" || vl == "unlimited" || vl == "0") s.turbo_speed = 0;
            else if (vl == "1.5x" || vl == "1.5" || vl == "15") s.turbo_speed = 15;
            else {
                const int n = std::atoi(vl.c_str());
                if (n == 2 || n == 3 || n == 4) s.turbo_speed = n;
            }
        }
        else if (k == "toast_theme") s.toast_theme = achievements::catalog_parse_toast_theme(v);
        else if (k == "hd_textures") s.hd_textures = parse_bool(v, true);
        else if (k == "hd_fmv") s.hd_fmv = parse_bool(v, true);
        else if (k == "mods_enabled") s.mods_enabled = parse_bool(v, true);
        else if (k == "tex_dump") s.tex_dump = parse_bool(v, false);
        else if (k == "fmv_dump") s.fmv_dump = parse_bool(v, false);
        else if (k == "texture_packs") s.texture_packs = v;
        else if (k == "video_packs") s.video_packs = v;
        else if (k == "active_mods") s.active_mods = v;
        else if (k == "launcher_hide_on_play") s.launcher_hide_on_play = parse_bool(v, true);
        else if (k == "launcher_chimes") s.launcher_chimes = parse_bool(v, true);
        else if (k == "launcher_hero_screenshot") s.launcher_hero_screenshot = parse_bool(v, true);
        else if (k == "launcher_fullscreen") s.launcher_fullscreen = parse_bool(v, false);
    }
    return s;
}

bool save_settings(const std::filesystem::path &keys_ini_path, const Settings &s) {
    std::string text = read_text_file(keys_ini_path);
    if (text.empty()) text = default_keys_ini_template();
    static const char *const kAspectWords[7] = {"4:3", "16:9", "stretch", "auto", "16:10", "21:9", "32:9"};
    const auto set = [&](const char *k, const std::string &val) { text = upsert_ini_setting(text, k, val); };

    set("scale", std::to_string(std::clamp(s.scale, 1, 64)));
    set("aspect", kAspectWords[std::clamp(s.aspect, 0, 6)]);
    set("true_widescreen", s.true_widescreen ? "on" : "off");
    set("fps_cap", s.fps_cap);
    set("fps_speed_sync", s.fps_speed_sync ? "on" : "off");
    set("gpu_api", s.gpu_api);
    set("present_mode", s.present_mode);
    set("look", s.look);
    set("sharp_2d", s.sharp_2d);
    set("sharp_3d", s.sharp_3d ? "on" : "off");
    set("deinterlace", s.deinterlace ? "on" : "off");
    set("gpu_binning", s.gpu_binning ? "on" : "off");
    set("adaptive_scale", s.adaptive_scale ? "on" : "off");
    set("perf_hud", s.perf_hud ? "on" : "off");
    set("scanlines", std::to_string(std::clamp(s.scanlines, 0, 100)));
    set("saturation", std::to_string(std::clamp(s.saturation, 0, 200)));
    set("ssao", std::to_string(std::clamp(s.ssao, 0, 100)));
    set("bloom", std::to_string(std::clamp(s.bloom, 0, 100)));
    set("ssgi", std::to_string(std::clamp(s.ssgi, 0, 100)));
    set("filmic", s.filmic ? "on" : "off");
    set("vignette", std::to_string(std::clamp(s.vignette, 0, 100)));
    set("master_volume", std::to_string(std::clamp(s.master_volume, 0, 100)));
    set("bgm_volume", std::to_string(std::clamp(s.bgm_volume, 0, 100)));
    set("sfx_volume", std::to_string(std::clamp(s.sfx_volume, 0, 100)));
    set("chime_volume", std::to_string(std::clamp(s.chime_volume, 0, 100)));
    set("mute", s.mute ? "on" : "off");
    set("audio_stretch", s.audio_stretch ? "on" : "off");
    set("fast_disc", s.fast_disc ? "on" : "off");
    set("autosave", s.autosave ? "on" : "off");
    set("turbo_speed", s.turbo_speed <= 0 ? "max" : (s.turbo_speed == 15 ? "1.5x" : (std::to_string(s.turbo_speed) + "x")));
    set("toast_theme", achievements::catalog_toast_theme_word(s.toast_theme));
    set("hd_textures", s.hd_textures ? "on" : "off");
    set("hd_fmv", s.hd_fmv ? "on" : "off");
    set("mods_enabled", s.mods_enabled ? "on" : "off");
    set("tex_dump", s.tex_dump ? "on" : "off");
    set("fmv_dump", s.fmv_dump ? "on" : "off");
    set("texture_packs", s.texture_packs);
    if (!s.video_packs.empty()) set("video_packs", s.video_packs);
    if (!s.active_mods.empty()) set("active_mods", s.active_mods);
    set("launcher_hide_on_play", s.launcher_hide_on_play ? "on" : "off");
    set("launcher_chimes", s.launcher_chimes ? "on" : "off");
    set("launcher_hero_screenshot", s.launcher_hero_screenshot ? "on" : "off");
    set("launcher_fullscreen", s.launcher_fullscreen ? "on" : "off");
    return write_text_atomic(keys_ini_path, text);
}

void load_trophies(LauncherState &state) {
    state.trophies.clear();
    state.trophies_unlocked = 0;
    std::memset(state.tier_unlocked, 0, sizeof state.tier_unlocked);
    std::memset(state.tier_total, 0, sizeof state.tier_total);
    state.best_ring_streak = 0;
    state.best_perfect_streak = 0;
    state.regions_visited = 0;

    if (!game_profile(state.game_id).has_achievement_catalog) return;

    const auto cat_game = (state.game_id == GameId::ShadowHearts1)
        ? achievements::CatalogGame::ShadowHearts1
        : (state.game_id == GameId::Koudelka
               ? achievements::CatalogGame::Koudelka
               : (state.game_id == GameId::FromTheNewWorld
                      ? achievements::CatalogGame::FromTheNewWorld
                      : achievements::CatalogGame::Covenant));
    const auto &items = achievements::catalog_items_for(cat_game);
    state.trophies.reserve(items.size());
    for (const auto &it : items) {
        TrophyEntry e;
        e.id = it.id;
        e.title = it.title;
        e.description = it.description;
        e.tier = it.tier;
        e.secret = it.secret;
        e.missable = it.missable;
        const int ti = std::clamp(static_cast<int>(e.tier), 0, 6);
        ++state.tier_total[ti];
        state.trophies.push_back(std::move(e));
    }

    const std::string text = read_text_file(state.paths.achievements_json);
    if (!text.empty()) {
        const auto find_int = [&](const std::string &key, int &out) {
            const std::size_t k = text.find("\"" + key + "\"");
            if (k == std::string::npos) return;
            const std::size_t c = text.find(':', k);
            if (c == std::string::npos) return;
            out = std::atoi(text.c_str() + c + 1);
        };
        find_int("best_ring_streak", state.best_ring_streak);
        find_int("best_perfect_streak", state.best_perfect_streak);

        if (const std::size_t rk = text.find("\"visited_regions\""); rk != std::string::npos) {
            const std::size_t lb = text.find('[', rk), rb = text.find(']', rk);
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                std::size_t p = lb + 1;
                while (p < rb) {
                    const std::size_t q1 = text.find('"', p);
                    if (q1 == std::string::npos || q1 >= rb) break;
                    const std::size_t q2 = text.find('"', q1 + 1);
                    if (q2 == std::string::npos || q2 >= rb) break;
                    if (q2 > q1 + 1) ++state.regions_visited;
                    p = q2 + 1;
                }
            }
        }

        std::unordered_map<std::string, std::string> burst_shots;
        if (const std::size_t bblk = text.find("\"burst_screenshots\""); bblk != std::string::npos) {
            const std::size_t lb = text.find('{', bblk);
            const std::size_t rb = (lb != std::string::npos) ? text.find('}', lb) : std::string::npos;
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                for (const TrophyEntry &e : state.trophies) {
                    const std::string needle = "\"" + e.id + "\"";
                    const std::size_t pos = text.find(needle, lb);
                    if (pos == std::string::npos || pos >= rb) continue;
                    const std::size_t colon = text.find(':', pos + needle.size());
                    if (colon == std::string::npos || colon >= rb) continue;
                    const std::size_t q1 = text.find('"', colon + 1);
                    if (q1 == std::string::npos || q1 >= rb) continue;
                    const std::size_t q2 = text.find('"', q1 + 1);
                    if (q2 == std::string::npos || q2 >= rb) continue;
                    burst_shots[e.id] = text.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        const std::size_t ublk = text.find("\"unlocked\"");
        const std::size_t lb_u = (ublk != std::string::npos) ? text.find('{', ublk) : std::string::npos;
        const std::size_t scope = (lb_u != std::string::npos) ? lb_u : ((ublk != std::string::npos) ? ublk : 0);
        const std::size_t rb_u = (lb_u != std::string::npos) ? text.find('}', lb_u) : text.size();
        const std::size_t limit = (rb_u != std::string::npos) ? rb_u : text.size();
        for (TrophyEntry &e : state.trophies) {
            const std::string needle = "\"" + e.id + "\"";
            const std::size_t pos = text.find(needle, scope);
            if (pos == std::string::npos || pos >= limit) continue;
            const std::size_t colon = text.find(':', pos + needle.size());
            if (colon == std::string::npos || colon >= limit) continue;
            const std::size_t q1 = text.find('"', colon + 1);
            if (q1 == std::string::npos || q1 >= limit) continue;
            const std::size_t q2 = text.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= limit) continue;
            e.unlocked = true;
            e.unlocked_at = text.substr(q1 + 1, q2 - q1 - 1);
            ++state.trophies_unlocked;
            const int ti = std::clamp(static_cast<int>(e.tier), 0, 6);
            ++state.tier_unlocked[ti];
        }

        // Match unlocked trophies to captured Achievement_*.png screenshots (individual first, then burst fallback)
        for (TrophyEntry &e : state.trophies) {
            if (!e.unlocked) continue;
            const std::string prefix = "Achievement_" + sanitize_filename(e.title) + "_";
            for (const auto &shot : state.screenshots) {
                if (shot.is_achievement && shot.filename.rfind(prefix, 0) == 0) {
                    e.screenshot_path = shot.path;
                    break;
                }
            }
            if (e.screenshot_path.empty()) {
                auto it = burst_shots.find(e.id);
                if (it != burst_shots.end() && !it->second.empty()) {
                    const std::string bprefix = "Achievement_" + sanitize_filename(it->second) + "_";
                    for (const auto &shot : state.screenshots) {
                        if (shot.is_achievement && shot.filename.rfind(bprefix, 0) == 0) {
                            e.screenshot_path = shot.path;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void load_save_slots(LauncherState &state) {
    state.occupied_slots = 0;
    state.latest_slot = -1;
    std::filesystem::file_time_type latest_time{};
    std::error_code ec;

    const auto read_one = [&](int s, const std::string &base, bool count_manual) -> SaveSlot {
        SaveSlot slot;
        slot.slot = s;
        slot.state_path = state.paths.states_dir / (base + ".state");
        const std::filesystem::path meta_path = state.paths.states_dir / (base + ".meta");
        const std::filesystem::path thumb_path = state.paths.states_dir / (base + ".thumb");

        // Auto-migrate legacy Koudelka root-level slot_N.state into states/slot_N.state
        if (state.game_id == GameId::Koudelka && s >= 0 && s < 10 &&
            !std::filesystem::is_regular_file(slot.state_path, ec)) {
            const std::filesystem::path legacy = state.paths.game_dir / (base + ".state");
            if (std::filesystem::is_regular_file(legacy, ec)) {
                std::filesystem::create_directories(state.paths.states_dir, ec);
                std::filesystem::rename(legacy, slot.state_path, ec);
                if (ec) {
                    ec.clear();
                    slot.state_path = legacy;
                }
            }
        }

        if (std::filesystem::is_regular_file(slot.state_path, ec)) {
            slot.exists = true;
            slot.bytes = static_cast<std::size_t>(std::filesystem::file_size(slot.state_path, ec));
            if (count_manual) ++state.occupied_slots;
            const auto mt = std::filesystem::last_write_time(slot.state_path, ec);
            if (!ec && (state.latest_slot < 0 || mt > latest_time)) {
                latest_time = mt;
                state.latest_slot = s;
            }
            if (!ec) slot.timestamp = format_file_time(mt);

            std::ifstream mf(meta_path);
            if (mf) {
                std::string line;
                while (std::getline(mf, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const std::size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string k = line.substr(0, eq);
                    const std::string v = line.substr(eq + 1);
                    if (k == "timestamp" && !v.empty()) slot.timestamp = v;
                    else if (k == "frame") slot.frame = std::strtoull(v.c_str(), nullptr, 10);
                    else if (k == "bytes" && slot.bytes == 0) slot.bytes = static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
                    else if (k == "disc") slot.disc = std::max(1, std::atoi(v.c_str()));
                    else if (k == "area") slot.area = v;
                    else if (k == "level") slot.max_level = std::atoi(v.c_str());
                    else if (k == "playtime") slot.playtime_sec = static_cast<float>(std::atof(v.c_str()));
                }
            }

            std::ifstream tf(thumb_path, std::ios::binary);
            if (tf) {
                std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
                static constexpr char kMagic[8] = {'P', 'S', '2', 'T', 'H', 'B', '1', 0};
                if (raw.size() >= 16 && std::memcmp(raw.data(), kMagic, 8) == 0) {
                    std::uint32_t tw = 0, th = 0;
                    std::memcpy(&tw, raw.data() + 8, 4);
                    std::memcpy(&th, raw.data() + 12, 4);
                    if (tw > 0 && th > 0 && tw <= 512 && th <= 512 &&
                        raw.size() >= 16u + static_cast<std::size_t>(tw) * th * 3u) {
                        slot.has_thumb = true;
                        slot.thumb_w = static_cast<int>(tw);
                        slot.thumb_h = static_cast<int>(th);
                        slot.thumb_bgra.resize(static_cast<std::size_t>(tw) * th);
                        const std::uint8_t *src = raw.data() + 16;
                        for (std::size_t i = 0; i < slot.thumb_bgra.size(); ++i) {
                            const std::uint32_t r = src[i * 3 + 0];
                            const std::uint32_t g = src[i * 3 + 1];
                            const std::uint32_t b = src[i * 3 + 2];
                            slot.thumb_bgra[i] = b | (g << 8) | (r << 16) | 0xFF000000u;
                        }
                    }
                }
            }
        }
        return slot;
    };

    for (int s = 0; s < 10; ++s) {
        state.slots[static_cast<std::size_t>(s)] = read_one(s, "slot_" + std::to_string(s), true);
    }
    state.autosave_slot = read_one(10, "autosave", false);
}

void load_screenshots(LauncherState &state) {
    state.screenshots.clear();
    state.hero_screenshot.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(state.paths.screenshots_dir, ec)) return;

    struct RankedShot {
        std::filesystem::file_time_type mtime;
        ScreenshotItem item;
    };
    std::vector<RankedShot> found;
    for (const auto &ent : std::filesystem::directory_iterator(state.paths.screenshots_dir, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        const auto &p = ent.path();
        const std::string ext = to_lower(p.extension().string());
        if (ext != ".png") continue;
        const std::string fname = p.filename().string();
        const std::string game_prefix = game_profile(state.game_id).screenshot_prefix;
        const bool is_ach = fname.rfind("Achievement_", 0) == 0;
        const bool is_game_shot = is_ach ||
                                  fname.rfind("ShadowHearts", 0) == 0 ||
                                  (!game_prefix.empty() && fname.rfind(game_prefix, 0) == 0);
        if (!is_game_shot) continue;

        ScreenshotItem item;
        item.path = p;
        item.filename = fname;
        item.is_achievement = is_ach;
        std::string disp = p.stem().string();
        if (is_ach && disp.size() > 12) disp = disp.substr(12);
        for (char &c : disp) if (c == '_') c = ' ';
        item.display_name = disp;
        const auto mt = ent.last_write_time(ec);
        item.date_text = ec ? "" : format_file_time(mt);
        found.push_back({mt, std::move(item)});
    }
    std::sort(found.begin(), found.end(), [](const RankedShot &a, const RankedShot &b) { return a.mtime > b.mtime; });
    for (auto &r : found) {
        if (state.hero_screenshot.empty()) state.hero_screenshot = r.item.path;
        state.screenshots.push_back(std::move(r.item));
    }
}

void evaluate_health(LauncherState &state) {
    state.health.clear();
    std::error_code ec;
    const GameProfile &prof = game_profile(state.game_id);
    const auto add = [&](const char *lbl, bool ok, const std::string &det) {
        state.health.push_back({lbl, det, ok});
    };

    const bool exe_ok = std::filesystem::is_regular_file(state.paths.game_exe, ec);
    state.exe_present = exe_ok;
    add("Game Executable", exe_ok,
        exe_ok ? (std::string("games/") + prof.slug + "/" + state.paths.game_exe.filename().string())
               : (std::string("Coming Soon — ") + prof.exe_name + " not built yet"));

    const bool d1_ok = !state.paths.disc1_path.empty() && std::filesystem::exists(state.paths.disc1_path, ec);
    add(prof.disc1_label, d1_ok,
        d1_ok ? (std::filesystem::is_directory(state.paths.disc1_path, ec)
                     ? ((prof.disc_count == 4)
                            ? "Unpacked 4-Disc SSD Library (disc_files, auto-swap ready)"
                            : ("Unpacked SSD folder (" + state.paths.disc1_path.filename().string() + ")"))
                     : state.paths.disc1_path.filename().string())
              : (std::string(prof.disc_count == 4 ? "Stage your 4 PS1 discs via Setup Menu -> Koudelka (games/" : "Stage your PS2 disc in games/") + prof.slug + "/"));

    if (prof.disc_count == 2) {
        const bool d2_ok = !state.paths.disc2_path.empty() && std::filesystem::exists(state.paths.disc2_path, ec);
        add(prof.disc2_label, d2_ok,
            d2_ok ? (std::filesystem::is_directory(state.paths.disc2_path, ec) ? "Unpacked SSD folder (disc2_files, auto-swap ready)" : state.paths.disc2_path.filename().string())
                  : "Optional until Disc 2 transition");
    }

    const bool sdl_ok = std::filesystem::is_regular_file(state.paths.root_dir / "SDL2.dll", ec);
    add("SDL2 Runtime", sdl_ok, sdl_ok ? "SDL2.dll present (shared in root)" : "Missing SDL2.dll");

    const bool dxc_ok = std::filesystem::is_regular_file(state.paths.root_dir / "dxcompiler.dll", ec) &&
                        std::filesystem::is_regular_file(state.paths.root_dir / "dxil.dll", ec);
    add("DXC Shader Compiler", dxc_ok, dxc_ok ? "dxcompiler.dll + dxil.dll ready (fast DX12 shaders)" : "Optional (missing dxcompiler.dll or dxil.dll; falls back to FXC)");

    const bool mc_ok = std::filesystem::is_regular_file(state.paths.memcard_path, ec);
    const bool is_ps1 = (state.game_id == GameId::Koudelka);
    add(is_ps1 ? "PS1 Memory Card" : "PS2 Memory Card", mc_ok,
        mc_ok ? (state.paths.memcard_path.filename().string() + " (" + format_bytes(std::filesystem::file_size(state.paths.memcard_path, ec)) + ")")
              : (is_ps1 ? "New 128 KB PS1 card will be created on first launch" : "New 8 MB card will be created on first launch"));

    const bool ini_ok = std::filesystem::is_regular_file(state.paths.keys_ini, ec);
    add("Configuration (keys.ini)", ini_ok, ini_ok ? "Active & synced with launcher" : "Defaults active (saved on first edit)");

    // Scan baked VU1 table vs captured vu1/*.bin recordings and vu1-live DLL cache
    state.vu1_cache = Vu1CacheSummary{};
    std::unordered_set<std::string> baked_hashes;
    const std::filesystem::path vu_table = state.paths.root_dir / "generated" / prof.slug / "vu1_table.cpp";
    if (std::filesystem::is_regular_file(vu_table, ec)) {
        std::ifstream f(vu_table);
        std::string line;
        while (std::getline(f, line)) {
            std::size_t pos = line.find("0x");
            while (pos != std::string::npos && pos + 21 <= line.size()) {
                bool hex_ok = true;
                std::string h;
                h.reserve(16);
                for (std::size_t k = 0; k < 16; ++k) {
                    const unsigned char ch = static_cast<unsigned char>(line[pos + 2 + k]);
                    if (!std::isxdigit(ch)) { hex_ok = false; break; }
                    h.push_back(static_cast<char>(std::toupper(ch)));
                }
                if (hex_ok && line.compare(pos + 18, 3, "ull") == 0) baked_hashes.insert(std::move(h));
                pos = line.find("0x", pos + 2);
            }
        }
    }
    state.vu1_cache.baked_programs = static_cast<int>(baked_hashes.size());
    std::unordered_set<std::string> captured_hashes;
    for (const char *sub : {"vu1", "vu0"}) {
        const std::filesystem::path dir = state.paths.game_dir / sub;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
            if (!ent.is_regular_file(ec) || to_lower(ent.path().extension().string()) != ".bin") continue;
            const std::string stem = ent.path().stem().string();
            if (stem.size() >= 18 && stem[16] == '_') {
                bool hex_ok = true;
                std::string h;
                h.reserve(16);
                for (std::size_t k = 0; k < 16; ++k) {
                    const unsigned char ch = static_cast<unsigned char>(stem[k]);
                    if (!std::isxdigit(ch)) { hex_ok = false; break; }
                    h.push_back(static_cast<char>(std::toupper(ch)));
                }
                if (hex_ok) captured_hashes.insert(std::move(h));
            }
        }
    }
    state.vu1_cache.captured_unique = static_cast<int>(captured_hashes.size());
    int unbaked = 0;
    for (const auto &h : captured_hashes) {
        if (baked_hashes.find(h) == baked_hashes.end()) ++unbaked;
    }
    state.vu1_cache.unbaked_count = unbaked;
    const std::filesystem::path live_dir = state.paths.game_dir / "vu1-live";
    if (std::filesystem::is_directory(live_dir, ec)) {
        for (const auto &ent : std::filesystem::directory_iterator(live_dir, ec)) {
            if (!ent.is_regular_file(ec)) continue;
            const std::string ext = to_lower(ent.path().extension().string());
            if (ext == ".dll" || ext == ".so" || ext == ".dylib") ++state.vu1_cache.live_dll_count;
        }
    }
    const bool vu_ok = (unbaked == 0);
    std::string vu_det;
    if (state.vu1_cache.baked_programs == 0 && state.vu1_cache.captured_unique == 0) {
        vu_det = "No VU1 captures yet (recorded automatically during play)";
    } else if (unbaked == 0) {
        vu_det = std::to_string(state.vu1_cache.baked_programs) + " baked into EXE — zero first-spell JIT stutter";
    } else {
        vu_det = std::to_string(state.vu1_cache.baked_programs) + " baked, " + std::to_string(unbaked) +
                 " new captured — click Bake VU1 to fold into EXE";
    }
    add("VU1 Microprogram Cache", vu_ok, vu_det);

    state.ready_to_play = exe_ok && d1_ok;
}

namespace {

std::vector<std::pair<std::string, bool>> parse_order_spec(const std::string &s) {
    std::vector<std::pair<std::string, bool>> spec;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t comma = s.find(',', pos);
        std::string tok = trim((comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos));
        if (!tok.empty()) {
            if (tok[0] == '!' && tok.size() > 1) spec.emplace_back(tok.substr(1), false);
            else spec.emplace_back(tok, true);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return spec;
}

std::string json_extract_string(const std::string &json, const char *key, const std::string &fallback = {}) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    const std::size_t col = json.find(':', pos + needle.size());
    if (col == std::string::npos) return fallback;
    const std::size_t q1 = json.find('"', col + 1);
    if (q1 == std::string::npos) return fallback;
    std::string out;
    for (std::size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char nx = json[++i];
            if (nx == 'n') out.push_back(' ');
            else out.push_back(nx);
        } else if (json[i] == '"') {
            break;
        } else {
            out.push_back(json[i]);
        }
    }
    return out.empty() ? fallback : out;
}

double json_extract_number(const std::string &json, const char *key, double fallback = 0.0) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    const std::size_t col = json.find(':', pos + needle.size());
    if (col == std::string::npos) return fallback;
    std::size_t i = col + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size()) return fallback;
    return std::atof(json.c_str() + i);
}

std::uint32_t crc32_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

}  // namespace

void load_texture_packs(LauncherState &state) {
    state.texture_packs.clear();
    state.dumped_tex_count = 0;
    state.dumped_fmv_count = 0;

    std::error_code ec;
    const auto spec = parse_order_spec(state.settings.texture_packs);

    // Discover pack directories under dedicated texture_packs/ first, then legacy mods/textures/
    std::unordered_map<std::string, TexturePackInfo> found;
    auto scan_tex_root = [&](const std::filesystem::path &root_p) {
        if (!std::filesystem::is_directory(root_p, ec)) return;
        for (const auto &ent : std::filesystem::directory_iterator(root_p, ec)) {
            if (!ent.is_directory(ec)) continue;
            const std::string name = ent.path().filename().string();
            if (found.find(name) != found.end()) continue;
            TexturePackInfo info;
            info.name = name;
            info.path = ent.path();
            info.enabled = true;
            for (const auto &it : std::filesystem::recursive_directory_iterator(ent.path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (!it.is_regular_file(ec)) continue;
                if (to_lower(it.path().extension().string()) == ".png") {
                    ++info.png_count;
                    info.bytes += std::filesystem::file_size(it.path(), ec);
                }
            }
            found.emplace(info.name, std::move(info));
        }
    };
    scan_tex_root(state.paths.texture_packs_dir);
    scan_tex_root(state.paths.mods_textures_dir);

    // Order by saved spec first, then append newly dropped packs alphabetically
    std::unordered_map<std::string, bool> placed;
    for (const auto &sp : spec) {
        auto it = found.find(sp.first);
        if (it != found.end() && !placed[sp.first]) {
            TexturePackInfo p = it->second;
            p.enabled = sp.second;
            state.texture_packs.push_back(std::move(p));
            placed[sp.first] = true;
        }
    }
    std::vector<std::string> remaining;
    for (const auto &kv : found) {
        if (!placed[kv.first]) remaining.push_back(kv.first);
    }
    std::sort(remaining.begin(), remaining.end());
    for (const auto &name : remaining) state.texture_packs.push_back(found[name]);

    // Count dumped textures and dumped FMVs
    if (std::filesystem::is_directory(state.paths.dumps_textures_dir, ec)) {
        for (const auto &it : std::filesystem::recursive_directory_iterator(state.paths.dumps_textures_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (it.is_regular_file(ec) && to_lower(it.path().extension().string()) == ".png") ++state.dumped_tex_count;
        }
    }
    if (std::filesystem::is_directory(state.paths.dumps_fmv_dir, ec)) {
        for (const auto &it : std::filesystem::directory_iterator(state.paths.dumps_fmv_dir, ec)) {
            if (!it.is_regular_file(ec)) continue;
            const std::string ext = to_lower(it.path().extension().string());
            if (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".m2v") ++state.dumped_fmv_count;
        }
    }

    load_video_packs(state);
    load_mods(state);
}

void load_video_packs(LauncherState &state) {
    state.video_packs.clear();
    state.fmv_mods = FmvModSummary{};
    std::error_code ec;
    const auto spec = parse_order_spec(state.settings.video_packs);

    std::unordered_map<std::string, VideoPackInfo> found;
    auto is_video_ext = [](const std::string &ext) {
        return ext == ".mp4" || ext == ".m4v" || ext == ".mov";
    };

    auto scan_video_root = [&](const std::filesystem::path &root_p, const char *loose_pack_name) {
        if (!std::filesystem::is_directory(root_p, ec)) return;
        VideoPackInfo loose;
        loose.name = loose_pack_name;
        loose.path = root_p;
        loose.enabled = true;

        for (const auto &ent : std::filesystem::directory_iterator(root_p, ec)) {
            if (ent.is_directory(ec)) {
                const std::string name = ent.path().filename().string();
                if (found.find(name) != found.end()) continue;
                VideoPackInfo vp;
                vp.name = name;
                vp.path = ent.path();
                vp.enabled = true;
                for (const auto &it : std::filesystem::recursive_directory_iterator(ent.path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
                    if (!it.is_regular_file(ec)) continue;
                    if (is_video_ext(to_lower(it.path().extension().string()))) {
                        ++vp.mp4_count;
                        vp.bytes += std::filesystem::file_size(it.path(), ec);
                    }
                }
                if (vp.mp4_count > 0) found.emplace(vp.name, std::move(vp));
            } else if (ent.is_regular_file(ec)) {
                if (is_video_ext(to_lower(ent.path().extension().string()))) {
                    ++loose.mp4_count;
                    loose.bytes += std::filesystem::file_size(ent.path(), ec);
                }
            }
        }
        if (loose.mp4_count > 0 && found.find(loose.name) == found.end()) {
            found.emplace(loose.name, std::move(loose));
        }
    };

    scan_video_root(state.paths.video_packs_dir, "HD_Video_Pack");
    scan_video_root(state.paths.mods_fmv_dir, "Legacy_FMV_Pack");

    std::unordered_map<std::string, bool> placed;
    for (const auto &sp : spec) {
        auto it = found.find(sp.first);
        if (it != found.end() && !placed[sp.first]) {
            VideoPackInfo p = it->second;
            p.enabled = sp.second;
            state.video_packs.push_back(std::move(p));
            placed[sp.first] = true;
        }
    }
    std::vector<std::string> remaining;
    for (const auto &kv : found) {
        if (!placed[kv.first]) remaining.push_back(kv.first);
    }
    std::sort(remaining.begin(), remaining.end());
    for (const auto &name : remaining) state.video_packs.push_back(found[name]);

    for (const auto &vp : state.video_packs) {
        if (vp.enabled) {
            state.fmv_mods.mp4_count += vp.mp4_count;
            state.fmv_mods.bytes += vp.bytes;
        }
    }
}

void load_mods(LauncherState &state) {
    state.mods.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(state.paths.mods_dir, ec)) return;

    const auto spec = parse_order_spec(state.settings.active_mods);
    std::unordered_map<std::string, ModInfo> found;

    for (const auto &ent : std::filesystem::directory_iterator(state.paths.mods_dir, ec)) {
        if (!ent.is_directory(ec)) continue;
        const std::string folder_name = ent.path().filename().string();
        const std::string low_name = to_lower(folder_name);
        // Skip legacy texture/FMV sub-directories inside mods/
        if (low_name == "textures" || low_name == "fmv") continue;

        ModInfo m;
        m.id = folder_name;
        m.name = folder_name;
        m.author = "Community";
        m.version = "1.0";
        m.path = ent.path();
        m.enabled = true;

        const std::string manifest = read_text_file(ent.path() / "mod.json");
        if (!manifest.empty()) {
            m.id = json_extract_string(manifest, "id", folder_name);
            m.name = json_extract_string(manifest, "name", folder_name);
            m.author = json_extract_string(manifest, "author", "Community");
            m.version = json_extract_string(manifest, "version", "1.0");
            m.description = json_extract_string(manifest, "description", "");
            m.priority = static_cast<int>(json_extract_number(manifest, "priority", 100.0));
        }

        // Layer 1: Virtual disc file overrides under files/
        const std::filesystem::path files_dir = ent.path() / "files";
        if (std::filesystem::is_directory(files_dir, ec)) {
            for (const auto &fit : std::filesystem::recursive_directory_iterator(files_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (!fit.is_regular_file(ec)) continue;
                ++m.file_override_count;
                std::string rel = std::filesystem::relative(fit.path(), files_dir, ec).generic_string();
                m.override_rel_paths.push_back(to_lower(rel));
            }
        }

        // Layer 2/3/4: .pnach patches, .lua scripts, .dll native plugins
        for (const auto &it : std::filesystem::recursive_directory_iterator(ent.path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (!it.is_regular_file(ec)) continue;
            const std::string ext = to_lower(it.path().extension().string());
            if (ext == ".pnach") {
                const std::string ptxt = read_text_file(it.path());
                std::istringstream pss(ptxt);
                std::string pline;
                while (std::getline(pss, pline)) {
                    const std::string tl = trim(pline);
                    if (tl.rfind("patch=", 0) == 0 || tl.rfind("patch =", 0) == 0) {
                        ++m.pnach_patch_count;
                        // Extract 3rd CSV field (address)
                        const std::size_t c1 = tl.find(',');
                        const std::size_t c2 = (c1 != std::string::npos) ? tl.find(',', c1 + 1) : std::string::npos;
                        if (c2 != std::string::npos) {
                            const std::uint32_t addr = static_cast<std::uint32_t>(std::strtoul(tl.c_str() + c2 + 1, nullptr, 16));
                            if (addr != 0) m.patched_addresses.push_back(addr);
                        }
                    }
                }
            } else if (ext == ".lua") {
                ++m.lua_hook_count;
            } else if (ext == ".dll") {
                ++m.dll_plugin_count;
            }
        }

        if (m.description.empty()) {
            std::string auto_desc;
            if (m.file_override_count > 0) auto_desc += std::to_string(m.file_override_count) + " file override(s)  ";
            if (m.pnach_patch_count > 0) auto_desc += std::to_string(m.pnach_patch_count) + " memory patch(es)  ";
            if (m.lua_hook_count > 0) auto_desc += std::to_string(m.lua_hook_count) + " Lua script(s)  ";
            if (m.dll_plugin_count > 0) auto_desc += std::to_string(m.dll_plugin_count) + " C++ plugin(s)";
            m.description = auto_desc.empty() ? "Nocturne gameplay modification" : trim(auto_desc);
        }

        found.emplace(folder_name, std::move(m));
    }

    std::unordered_map<std::string, bool> placed;
    for (const auto &sp : spec) {
        auto it = found.find(sp.first);
        if (it != found.end() && !placed[sp.first]) {
            ModInfo m = it->second;
            m.enabled = sp.second;
            state.mods.push_back(std::move(m));
            placed[sp.first] = true;
        }
    }
    std::vector<ModInfo> rem;
    for (auto &kv : found) {
        if (!placed[kv.first]) rem.push_back(std::move(kv.second));
    }
    std::sort(rem.begin(), rem.end(), [](const ModInfo &a, const ModInfo &b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.name < b.name;
    });
    for (auto &m : rem) state.mods.push_back(std::move(m));

    // Conflict detection across enabled mods (file override collisions & memory patch address collisions)
    for (std::size_t i = 0; i < state.mods.size(); ++i) {
        if (!state.mods[i].enabled) continue;
        for (std::size_t j = i + 1; j < state.mods.size(); ++j) {
            if (!state.mods[j].enabled) continue;
            bool conflict = false;
            for (const auto &f1 : state.mods[i].override_rel_paths) {
                if (std::find(state.mods[j].override_rel_paths.begin(),
                              state.mods[j].override_rel_paths.end(), f1) != state.mods[j].override_rel_paths.end()) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict) {
                for (std::uint32_t a1 : state.mods[i].patched_addresses) {
                    if (std::find(state.mods[j].patched_addresses.begin(),
                                  state.mods[j].patched_addresses.end(), a1) != state.mods[j].patched_addresses.end()) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (conflict) {
                state.mods[i].conflicts_with.push_back(state.mods[j].name);
                state.mods[j].conflicts_with.push_back(state.mods[i].name);
            }
        }
    }
}

void load_playtime(LauncherState &state) {
    state.playtime = PlaytimeStats{};
    const std::string j = read_text_file(state.paths.playtime_json);
    if (!j.empty()) {
        state.playtime.solo_seconds = json_extract_number(j, "solo_seconds", 0.0);
        state.playtime.coop_seconds = json_extract_number(j, "coop_seconds", 0.0);
        state.playtime.sessions_count = static_cast<int>(json_extract_number(j, "sessions_count", 0.0));
        state.playtime.last_played = json_extract_string(j, "last_played", "");
    }
    double max_slot_sec = 0.0;
    for (const auto &sl : state.slots) {
        if (sl.exists && sl.playtime_sec > max_slot_sec) max_slot_sec = sl.playtime_sec;
    }
    if (state.autosave_slot.exists && state.autosave_slot.playtime_sec > max_slot_sec) {
        max_slot_sec = state.autosave_slot.playtime_sec;
    }
    state.playtime.effective_total_seconds = std::max(state.playtime.solo_seconds, max_slot_sec) + state.playtime.coop_seconds;
}

bool record_playtime_session(const LauncherPaths &paths, double elapsed_seconds, bool is_coop) {
    if (elapsed_seconds < 0.0) elapsed_seconds = 0.0;
    double solo = 0.0, coop = 0.0;
    int sessions = 0;
    const std::string existing = read_text_file(paths.playtime_json);
    if (!existing.empty()) {
        solo = json_extract_number(existing, "solo_seconds", 0.0);
        coop = json_extract_number(existing, "coop_seconds", 0.0);
        sessions = static_cast<int>(json_extract_number(existing, "sessions_count", 0.0));
    }
    if (is_coop) coop += elapsed_seconds;
    else solo += elapsed_seconds;
    ++sessions;
    const std::string stamp = now_file_stamp();
    char buf[384];
    std::snprintf(buf, sizeof buf,
                  "{\n"
                  "  \"solo_seconds\": %.2f,\n"
                  "  \"coop_seconds\": %.2f,\n"
                  "  \"sessions_count\": %d,\n"
                  "  \"last_played\": \"%s\"\n"
                  "}\n",
                  solo, coop, sessions, stamp.c_str());
    return write_text_atomic(paths.playtime_json, buf);
}

std::string serialize_texture_packs(const std::vector<TexturePackInfo> &packs) {
    std::string out;
    for (std::size_t i = 0; i < packs.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (!packs[i].enabled) out.push_back('!');
        out += packs[i].name;
    }
    return out;
}

bool toggle_texture_pack(LauncherState &state, std::size_t index) {
    if (index >= state.texture_packs.size()) return false;
    state.texture_packs[index].enabled = !state.texture_packs[index].enabled;
    state.settings.texture_packs = serialize_texture_packs(state.texture_packs);
    return true;
}

bool move_texture_pack(LauncherState &state, std::size_t index, int delta) {
    if (index >= state.texture_packs.size() || delta == 0) return false;
    const int target = static_cast<int>(index) + delta;
    if (target < 0 || target >= static_cast<int>(state.texture_packs.size())) return false;
    std::swap(state.texture_packs[index], state.texture_packs[static_cast<std::size_t>(target)]);
    state.settings.texture_packs = serialize_texture_packs(state.texture_packs);
    return true;
}

std::string serialize_video_packs(const std::vector<VideoPackInfo> &packs) {
    std::string out;
    for (std::size_t i = 0; i < packs.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (!packs[i].enabled) out.push_back('!');
        out += packs[i].name;
    }
    return out;
}

bool toggle_video_pack(LauncherState &state, std::size_t index) {
    if (index >= state.video_packs.size()) return false;
    state.video_packs[index].enabled = !state.video_packs[index].enabled;
    state.settings.video_packs = serialize_video_packs(state.video_packs);
    state.fmv_mods = FmvModSummary{};
    for (const auto &vp : state.video_packs) {
        if (vp.enabled) {
            state.fmv_mods.mp4_count += vp.mp4_count;
            state.fmv_mods.bytes += vp.bytes;
        }
    }
    return true;
}

bool move_video_pack(LauncherState &state, std::size_t index, int delta) {
    if (index >= state.video_packs.size() || delta == 0) return false;
    const int target = static_cast<int>(index) + delta;
    if (target < 0 || target >= static_cast<int>(state.video_packs.size())) return false;
    std::swap(state.video_packs[index], state.video_packs[static_cast<std::size_t>(target)]);
    state.settings.video_packs = serialize_video_packs(state.video_packs);
    return true;
}

std::string serialize_mods(const std::vector<ModInfo> &mods) {
    std::string out;
    for (std::size_t i = 0; i < mods.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (!mods[i].enabled) out.push_back('!');
        out += mods[i].path.filename().string();
    }
    return out;
}

bool toggle_mod(LauncherState &state, std::size_t index) {
    if (index >= state.mods.size()) return false;
    state.mods[index].enabled = !state.mods[index].enabled;
    state.settings.active_mods = serialize_mods(state.mods);
    load_mods(state);  // Re-evaluate conflicts
    return true;
}

bool move_mod(LauncherState &state, std::size_t index, int delta) {
    if (index >= state.mods.size() || delta == 0) return false;
    const int target = static_cast<int>(index) + delta;
    if (target < 0 || target >= static_cast<int>(state.mods.size())) return false;
    std::swap(state.mods[index], state.mods[static_cast<std::size_t>(target)]);
    state.settings.active_mods = serialize_mods(state.mods);
    return true;
}

void refresh_all(LauncherState &state) {
    state.paths = resolve_paths(state.paths.root_dir, state.game_id);
    state.settings = load_settings(state.paths.keys_ini);
    load_screenshots(state);  // First so load_trophies can match captured achievement screenshots
    load_trophies(state);
    load_save_slots(state);
    load_playtime(state);
    load_texture_packs(state);
    evaluate_health(state);
}

std::array<TrilogyCardSummary, static_cast<std::size_t>(GameId::Count)> load_trilogy_summaries(const std::filesystem::path &root_dir) {
    std::array<TrilogyCardSummary, static_cast<std::size_t>(GameId::Count)> out{};
    std::error_code ec;
    for (int i = 0; i < static_cast<int>(GameId::Count); ++i) {
        const GameId gid = static_cast<GameId>(i);
        LauncherState tmp;
        tmp.game_id = gid;
        tmp.paths = resolve_paths(root_dir, gid);
        tmp.settings = load_settings(tmp.paths.keys_ini);
        load_screenshots(tmp);
        load_trophies(tmp);
        load_save_slots(tmp);
        load_playtime(tmp);
        load_mods(tmp);

        TrilogyCardSummary &s = out[static_cast<std::size_t>(i)];
        s.id = gid;
        s.exe_present = std::filesystem::is_regular_file(tmp.paths.game_exe, ec);
        s.disc1_ok = !tmp.paths.disc1_path.empty() && std::filesystem::exists(tmp.paths.disc1_path, ec);
        s.disc2_ok = !tmp.paths.disc2_path.empty() && std::filesystem::exists(tmp.paths.disc2_path, ec);
        s.ready_to_play = s.exe_present && s.disc1_ok;
        s.occupied_slots = tmp.occupied_slots;
        s.has_autosave = tmp.autosave_slot.exists;
        s.trophies_unlocked = tmp.trophies_unlocked;
        s.trophies_total = static_cast<int>(tmp.trophies.size());
        s.playtime_seconds = tmp.playtime.effective_total_seconds;
        s.screenshot_count = static_cast<int>(tmp.screenshots.size());
        int active_mods = 0;
        for (const auto &m : tmp.mods) if (m.enabled) ++active_mods;
        s.active_mod_count = active_mods;
        if (tmp.latest_slot == 10 && tmp.autosave_slot.exists) {
            s.latest_area = tmp.autosave_slot.area;
        } else if (tmp.latest_slot >= 0 && tmp.latest_slot < 10 && tmp.slots[static_cast<std::size_t>(tmp.latest_slot)].exists) {
            s.latest_area = tmp.slots[static_cast<std::size_t>(tmp.latest_slot)].area;
        }
        s.cover_image = tmp.paths.cover_image;
        s.hero_screenshot = tmp.hero_screenshot;
    }
    return out;
}

bool delete_save_slot(const LauncherPaths &paths, int slot) {
    if (slot < 0 || slot > 10) return false;
    std::error_code ec;
    const std::string base = (slot == 10) ? std::string("autosave") : ("slot_" + std::to_string(slot));
    const std::filesystem::path st = paths.states_dir / (base + ".state");
    const std::filesystem::path legacy = paths.game_dir / (base + ".state");
    const bool existed = std::filesystem::exists(st, ec) || std::filesystem::exists(legacy, ec);
    std::filesystem::remove(st, ec);
    std::filesystem::remove(legacy, ec);
    std::filesystem::remove(paths.states_dir / (base + ".meta"), ec);
    std::filesystem::remove(paths.states_dir / (base + ".thumb"), ec);
    std::filesystem::remove(paths.states_dir / (base + ".state.ps2"), ec);
    return existed;
}

bool backup_memcard(const LauncherPaths &paths, std::filesystem::path *out_backup_path, std::string *out_msg) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(paths.memcard_path, ec)) {
        if (out_msg) *out_msg = "No " + paths.memcard_path.filename().string() + " found yet to back up.";
        return false;
    }
    std::filesystem::create_directories(paths.backups_dir, ec);
    if (ec) {
        if (out_msg) *out_msg = "Could not create backups folder.";
        return false;
    }
    const std::string ext = paths.memcard_path.extension().empty() ? ".ps2" : paths.memcard_path.extension().string();
    const std::filesystem::path dst = paths.backups_dir / ("memcard_" + now_file_stamp() + ext);
    std::filesystem::copy_file(paths.memcard_path, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (out_msg) *out_msg = "Backup copy failed: " + ec.message();
        return false;
    }
    if (out_backup_path) *out_backup_path = dst;
    if (out_msg) *out_msg = "Memory card backed up to backups/" + dst.filename().string();
    return true;
}

bool export_save_bundle(const LauncherPaths &paths, std::filesystem::path *out_bundle_path, std::string *out_msg) {
    std::error_code ec;
    struct Entry {
        std::string rel_path;
        std::vector<std::uint8_t> data;
        std::uint32_t crc = 0;
        std::uint32_t local_hdr_offset = 0;
    };
    std::vector<Entry> entries;

    auto add_file_if_exists = [&](const std::filesystem::path &fp, const std::string &rel) {
        if (!std::filesystem::is_regular_file(fp, ec)) return;
        std::ifstream ifs(fp, std::ios::binary);
        if (!ifs) return;
        Entry e;
        e.rel_path = rel;
        e.data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        e.crc = crc32_bytes(e.data.data(), e.data.size());
        entries.push_back(std::move(e));
    };

    add_file_if_exists(paths.memcard_path, paths.memcard_path.filename().string());
    add_file_if_exists(paths.achievements_json, "achievements.json");
    add_file_if_exists(paths.playtime_json, "playtime.json");
    add_file_if_exists(paths.keys_ini, "keys.ini");

    if (std::filesystem::is_directory(paths.states_dir, ec)) {
        for (const auto &it : std::filesystem::directory_iterator(paths.states_dir, ec)) {
            if (!it.is_regular_file(ec)) continue;
            add_file_if_exists(it.path(), "states/" + it.path().filename().string());
        }
    }

    if (entries.empty()) {
        if (out_msg) *out_msg = "No memory card, save states, or config files found to export yet.";
        return false;
    }

    std::filesystem::create_directories(paths.backups_dir, ec);
    const std::string slug = paths.game_dir.filename().string();
    const std::filesystem::path zip_path = paths.backups_dir / (slug + "_" + now_file_stamp() + ".nocturne-save.zip");
    std::ofstream ofs(zip_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (out_msg) *out_msg = "Failed to open save bundle for writing.";
        return false;
    }

    auto wr16 = [&](std::uint16_t v) { ofs.write(reinterpret_cast<const char *>(&v), 2); };
    auto wr32 = [&](std::uint32_t v) { ofs.write(reinterpret_cast<const char *>(&v), 4); };

    for (auto &e : entries) {
        e.local_hdr_offset = static_cast<std::uint32_t>(ofs.tellp());
        wr32(0x04034B50u);  // Local file header signature
        wr16(20u);          // Version needed
        wr16(0u);           // Flags
        wr16(0u);           // Compression method 0 (Store)
        wr16(0u);           // Mod time
        wr16(0u);           // Mod date
        wr32(e.crc);
        wr32(static_cast<std::uint32_t>(e.data.size()));
        wr32(static_cast<std::uint32_t>(e.data.size()));
        wr16(static_cast<std::uint16_t>(e.rel_path.size()));
        wr16(0u);           // Extra len
        ofs.write(e.rel_path.data(), static_cast<std::streamsize>(e.rel_path.size()));
        if (!e.data.empty()) {
            ofs.write(reinterpret_cast<const char *>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
        }
    }

    const std::uint32_t cd_offset = static_cast<std::uint32_t>(ofs.tellp());
    for (const auto &e : entries) {
        wr32(0x02014B50u);  // Central directory file header signature
        wr16(20u);          // Version made by
        wr16(20u);          // Version needed
        wr16(0u);           // Flags
        wr16(0u);           // Method 0 (Store)
        wr16(0u);           // Mod time
        wr16(0u);           // Mod date
        wr32(e.crc);
        wr32(static_cast<std::uint32_t>(e.data.size()));
        wr32(static_cast<std::uint32_t>(e.data.size()));
        wr16(static_cast<std::uint16_t>(e.rel_path.size()));
        wr16(0u);           // Extra len
        wr16(0u);           // Comment len
        wr16(0u);           // Disk start
        wr16(0u);           // Internal attr
        wr32(0u);           // External attr
        wr32(e.local_hdr_offset);
        ofs.write(e.rel_path.data(), static_cast<std::streamsize>(e.rel_path.size()));
    }
    const std::uint32_t cd_size = static_cast<std::uint32_t>(ofs.tellp()) - cd_offset;

    wr32(0x06054B50u);  // End of central directory signature
    wr16(0u);
    wr16(0u);
    wr16(static_cast<std::uint16_t>(entries.size()));
    wr16(static_cast<std::uint16_t>(entries.size()));
    wr32(cd_size);
    wr32(cd_offset);
    wr16(0u);
    ofs.flush();

    if (!ofs) {
        if (out_msg) *out_msg = "Failed to finalize save bundle.";
        return false;
    }
    if (out_bundle_path) *out_bundle_path = zip_path;
    if (out_msg) *out_msg = "Exported " + std::to_string(entries.size()) + " file(s) to backups/" + zip_path.filename().string();
    return true;
}

bool import_save_bundle(const LauncherPaths &paths, const std::filesystem::path &bundle_path, std::string *out_msg) {
    std::ifstream ifs(bundle_path, std::ios::binary);
    if (!ifs) {
        if (out_msg) *out_msg = "Could not open save bundle: " + bundle_path.filename().string();
        return false;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    int restored = 0;
    std::error_code ec;

    while (pos + 30 <= raw.size()) {
        std::uint32_t sig = 0;
        std::memcpy(&sig, raw.data() + pos, 4);
        if (sig != 0x04034B50u) break;

        std::uint16_t method = 0, name_len = 0, extra_len = 0;
        std::uint32_t comp_size = 0, uncomp_size = 0, expected_crc = 0;
        std::memcpy(&method, raw.data() + pos + 8, 2);
        std::memcpy(&expected_crc, raw.data() + pos + 14, 4);
        std::memcpy(&comp_size, raw.data() + pos + 18, 4);
        std::memcpy(&uncomp_size, raw.data() + pos + 22, 4);
        std::memcpy(&name_len, raw.data() + pos + 26, 2);
        std::memcpy(&extra_len, raw.data() + pos + 28, 2);

        const std::size_t data_off = pos + 30 + name_len + extra_len;
        if (method != 0 || data_off + comp_size > raw.size()) {
            if (out_msg) *out_msg = "Unsupported or corrupted .nocturne-save.zip entry.";
            return false;
        }
        std::string rel(reinterpret_cast<const char *>(raw.data() + pos + 30), name_len);
        // Sanitize against path traversal
        if (rel.find("..") == std::string::npos && !rel.empty() && rel[0] != '/' && rel[0] != '\\') {
            const std::uint32_t actual_crc = crc32_bytes(raw.data() + data_off, comp_size);
            if (actual_crc != expected_crc) {
                if (out_msg) *out_msg = "CRC mismatch in save bundle entry: " + rel;
                return false;
            }
            const std::filesystem::path dst = paths.game_dir / rel;
            if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream ofs(dst, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(reinterpret_cast<const char *>(raw.data() + data_off), static_cast<std::streamsize>(comp_size));
                if (ofs.good()) ++restored;
            }
        }
        pos = data_off + comp_size;
    }

    if (restored == 0) {
        if (out_msg) *out_msg = "No valid files found in save bundle.";
        return false;
    }
    if (out_msg) *out_msg = "Restored " + std::to_string(restored) + " file(s) from " + bundle_path.filename().string();
    return true;
}

int clear_live_caches(const LauncherPaths &paths) {
    int removed = 0;
    std::error_code ec;
    for (const char *sub : {"ee-live", "vu1-live"}) {
        const std::filesystem::path dir = paths.game_dir / sub;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
            if (ent.is_directory(ec)) {
                const auto cnt = std::filesystem::remove_all(ent.path(), ec);
                if (!ec && cnt > 0 && cnt != static_cast<std::uintmax_t>(-1)) removed += static_cast<int>(cnt);
            } else if (ent.is_regular_file(ec)) {
                if (std::filesystem::remove(ent.path(), ec)) ++removed;
            }
        }
    }
    return removed;
}

std::wstring build_launch_command(const LauncherPaths &paths, bool fullscreen, int load_slot,
                                  const std::string &extra_args) {
    std::wstring cmd = quote_arg(paths.game_exe.wstring());
    if (!paths.disc1_path.empty()) cmd += L" --disc " + quote_arg(paths.disc1_path.wstring());
    if (!paths.disc2_path.empty()) cmd += L" --disc2 " + quote_arg(paths.disc2_path.wstring());
    cmd += L" --game-dir " + quote_arg(paths.game_dir.wstring());
    cmd += L" --memcard " + quote_arg(paths.memcard_path.wstring());
    cmd += fullscreen ? L" --fullscreen" : L" --window";
    if (load_slot == 10) cmd += L" --load-slot auto";
    else if (load_slot >= 0 && load_slot <= 9) cmd += L" --load-slot " + std::to_wstring(load_slot);
    if (!extra_args.empty()) {
        cmd += L" ";
        cmd += std::wstring(extra_args.begin(), extra_args.end());
    }
    return cmd;
}

std::string format_playtime(float seconds) {
    if (seconds <= 0.0f) return "0h 00m";
    const int total_min = static_cast<int>(seconds / 60.0f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%dh %02dm", total_min / 60, total_min % 60);
    return buf;
}

std::string format_bytes(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / static_cast<double>(1ull << 20));
    else if (bytes >= 1024ull) std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

}  // namespace launcher
