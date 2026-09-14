// scum_rcon.h -- aggregate project header + shared helpers
//
// Public surface of the scum_rcon namespaces (server / dispatch / game_thread
// / engine_hooks / questdb / env_scan / config). Only the symbols that the
// installed UE4SS.dll exports are referenced; see UE4SSStubs/UE4SS.h.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <UE4SS.h>

// ---------------------------------------------------------------------------
//  Logging helper -- routes a wide string to every UE4SS output device.
// ---------------------------------------------------------------------------
namespace scum_rcon::log
{
    inline void raw(std::wstring_view msg)
    {
        RC::Output::send(msg);
    }

    // Append a "[SCUM-RCON] " prefix and a newline.
    inline void line(std::wstring_view msg)
    {
        RC::Output::send(L"[SCUM-RCON] ");
        RC::Output::send(msg);
        RC::Output::send(L"\n");
    }

    // Narrow/wide conversion helpers.
    inline std::wstring widen(std::string const& s)
    {
        std::wstring out;
        out.reserve(s.size());
        for (unsigned char c : s) out.push_back(static_cast<wchar_t>(c));
        return out;
    }

    inline std::string narrow(std::wstring const& s)
    {
        std::string out;
        out.reserve(s.size());
        for (wchar_t c : s) out.push_back(static_cast<char>(c & 0xFF));
        return out;
    }
} // namespace scum_rcon::log

// ---------------------------------------------------------------------------
//  DLL exports consumed by UE4SS (defined in dllmain.cpp)
// ---------------------------------------------------------------------------
extern "C" {
    __declspec(dllexport) void* start_mod();
    __declspec(dllexport) void  uninstall_mod(void* mod);
}
