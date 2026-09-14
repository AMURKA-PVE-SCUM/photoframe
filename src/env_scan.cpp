// env_scan.cpp -- AV/EDR inline-hook detection
//
// Walks the module list of the current process (ScumServer.exe) and flags
// known inline-hooking AV/EDR products. These products patch function
// prologues in-process and can destabilise UE4SS/RCON inline hooks.
#include "env_scan.h"
#include "scum_rcon.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

namespace scum_rcon::env_scan {

namespace {

bool iequals(std::string const& a, std::string const& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Known module names (case-insensitive, DLL part only).
constexpr const char* kKnown[] = {
    "aswhook.dll",       // Bitdefender
    "avast!",
    "avg",
    "avghookx.dll",
    "avgsnx.dll",
    "bdhook.dll",
    "csagent.dll",       // CrowdStrike
    "crowdstrike",
    "elam",
    "endpoint",
    "epsecurity",
    "epp",
    "fsecure",
    "hyperscan",
    "kaspersky",
    "klif.sys",
    "mcafee",
    "nanowall",
    "senseir",
    "sentinelagent",
    "symc",
    "trend",
    "win32kfull",
};

} // anonymous namespace

std::vector<SecurityModule> scan()
{
    std::vector<SecurityModule> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            std::wstring wname(me.szModule);
            std::string  name = log::narrow(wname);

            std::string lower;
            lower.reserve(name.size());
            for (char c : name)
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            for (const char* k : kKnown) {
                if (lower.find(k) != std::string::npos) {
                    out.push_back(SecurityModule{ name, me.modBaseAddr, me.modBaseSize });
                    break;
                }
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

} // namespace scum_rcon::env_scan
