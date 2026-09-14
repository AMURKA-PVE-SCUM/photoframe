// config.cpp -- config.ini parsing
#include "config.h"
#include "scum_rcon.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace scum_rcon::config {

namespace {

std::string trim(std::string s)
{
    auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> split_csv(std::string s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { auto t = trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); }
        else cur.push_back(c);
    }
    auto t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

// Path of the module containing this function (i.e. main.dll).
std::wstring this_module_path()
{
    HMODULE h = nullptr;
    wchar_t buf[MAX_PATH * 2] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&this_module_path), &h)) {
        DWORD n = GetModuleFileNameW(h, buf, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    }
    return {};
}

// Locate config.ini. Preferred location: the mod folder itself (this DLL
// lives at <mod>\dlls\main.dll, so the mod folder is the DLL's parent).
// Fall back to walking up from the process working directory.
std::string find_config_ini_path()
{
    namespace fs = std::filesystem;
    auto mod_path = this_module_path();
    if (!mod_path.empty()) {
        auto dlls_dir = fs::path(mod_path).parent_path(); // <mod>\dlls
        auto cand = dlls_dir.parent_path() / "config.ini"; // <mod>\config.ini
        if (fs::exists(cand)) return cand.string();
        auto cand2 = dlls_dir / "config.ini";
        if (fs::exists(cand2)) return cand2.string();
    }
    auto dir = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        auto candidate = dir / "config.ini";
        if (fs::exists(candidate)) return candidate.string();
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    // Fall back: write next to the mod DLL directory if reachable.
    return (fs::current_path() / "config.ini").string();
}

constexpr const char* kDefaultTemplate = R"INI(; SCUM-RCON configuration
; ----------------------------------------

[rcon]
; The RCON listener will NOT start until 'password' is changed from
; its default value. Set a strong password before exposing this.
bind_address    = 0.0.0.0
port            = 25575
password        = CHANGE_ME_BEFORE_USE
application_id  =
auth_log        = true
analysis_limit  = 0

[quests]
; Quests that cause the login-lockout (their interactables overflow the
; client's reliable buffer on spawn, kicking the player every login).
; Comma-separated quest names: vanilla 'T3_DC_Interact_ScanAbandonedCity',
; a 'Quests/Override/Foo.json' path, or a bare stem all work (matched by
; stem). FindQuestLockouts lists players still holding one of these.
blocked         =
; Delete blocked-quest rows automatically at boot.
; OFF by default. Manual trigger any time: RunQuestUnstick.
auto_unstick             = false
; auto_unstick_dry_run: log what would be deleted, delete nothing.
auto_unstick_dry_run     = false
; auto_unstick_max_delete: abort the sweep if more rows match (a
; circuit breaker against a too-broad blocked list).
auto_unstick_max_delete  = 100

[logging]
verbose = false
path    =

[synth]
; Fabricate a synthetic ConZPlayerController + pawn so commands that
; enumerate world controllers (e.g. listplayers) complete headless.
; WARNING: spawning a player controller + pawn and possessing on a dedicated
; server can crash it. OFF by default.
pc = false
)INI";

} // anonymous namespace

Config& current()
{
    static Config g_cfg;
    return g_cfg;
}

bool load(Config& out)
{
    std::string path = find_config_ini_path();
    std::ifstream f(path);
    if (!f) {
        log::line(L"config.ini not found - a default template was written; set a password and restart");
        std::ofstream out_default(path);
        out_default << kDefaultTemplate;
        return false;
    }

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        if (section == "rcon") {
            if      (key == "bind_address")   out.rcon.bind_address = val;
            else if (key == "port")           out.rcon.port = static_cast<std::uint16_t>(std::stoi(val));
            else if (key == "password")       out.rcon.password = val;
            else if (key == "application_id") out.rcon.application_id = val;
            else if (key == "auth_log")       out.rcon.auth_log = (val == "true" || val == "1");
            else if (key == "analysis_limit") out.rcon.analysis_limit = static_cast<std::size_t>(std::stoull(val));
        } else if (section == "quests") {
            if      (key == "blocked")                  out.quests.blocked = split_csv(val);
            else if (key == "auto_unstick")             out.quests.auto_unstick = (val == "true" || val == "1");
            else if (key == "auto_unstick_dry_run")     out.quests.auto_unstick_dry_run = (val == "true" || val == "1");
            else if (key == "auto_unstick_max_delete")  out.quests.auto_unstick_max_delete = static_cast<std::size_t>(std::stoull(val));
        } else if (section == "logging") {
            if      (key == "verbose") out.logging.verbose = (val == "true" || val == "1");
            else if (key == "path")    out.logging.path = val;
        } else if (section == "synth") {
            if      (key == "pc") out.synth.pc = (val == "true" || val == "1");
        }
    }

    if (out.quests.blocked.empty()) {
        log::line(L"no quests configured - set [quests] blocked in config.ini");
    }

    return true;
}

} // namespace scum_rcon::config
