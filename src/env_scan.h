// env_scan.h -- AV/EDR inline-hook detection
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace scum_rcon::env_scan {

struct SecurityModule {
    std::string name;
    void*       base = nullptr;
    std::size_t size = 0;
};

// Scans the host process module list for known inline-hooking AV/EDR products.
std::vector<SecurityModule> scan();

} // namespace scum_rcon::env_scan
