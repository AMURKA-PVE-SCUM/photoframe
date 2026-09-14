// synth.h -- synthetic caller fabrication (headless command execution)
#pragma once
#include "scum_rcon.h"

namespace scum_rcon::synth {

// Find the current game UWorld (type UWorld).
RC::Unreal::UObject* find_main_world();

// Get (cached) or build a synthetic ConZPlayerController + ConZCharacter
// (possessed) so commands that enumerate the world's controllers / need a
// BP caller context can complete headless. Returns the controller, or nullptr
// on failure.
RC::Unreal::UObject* get_or_build_companion_controller();

} // namespace scum_rcon::synth
