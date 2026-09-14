// engine_hooks.h -- engine-side hooks (RegisterHook-based)
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace scum_rcon::engine_hooks {

struct HookStats {
    std::size_t installed      = 0;
    std::size_t pattern_misses = 0;
};

// Installs all engine-side hooks (chat-line capture, admin-output capture,
// shutdown hook). Pattern-scan/detour hooks (authorization, vehicle-list)
// are reported as unavailable -- they require internal engine signatures.
HookStats install_all();

bool install_chat_line_capture();
bool install_admin_output_capture();
bool install_shutdown_hook();

} // namespace scum_rcon::engine_hooks