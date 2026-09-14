// game_thread.h -- engine-tick command drain
#pragma once
#include <functional>
#include <string>

namespace scum_rcon::game_thread {

// Installs an EngineTick pre-callback (primary) or a ProcessEvent pre-callback
// (fallback) that drains the RCON command queue on the game thread.
bool install_tick_drain();

// Enqueue a command for the game-thread drain. The on_complete callback is
// invoked on the game thread with the reply string.
void enqueue(std::string cmdline, std::function<void(std::string)> on_complete);

// Drain at most one queued command (also invoked from RconMod::on_update).
void tick();

} // namespace scum_rcon::game_thread
