// config.h -- config.ini parsing
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scum_rcon::config {

struct RconConfig {
    std::string  bind_address = "0.0.0.0";
    std::uint16_t port        = 25575;
    std::string  password     = "CHANGE_ME_BEFORE_USE";
    std::string  application_id;
    bool         auth_log       = true;
    std::size_t  analysis_limit = 0;
};

struct QuestsConfig {
    std::vector<std::string> blocked;
    bool         auto_unstick            = false;
    bool         auto_unstick_dry_run    = false;
    std::size_t  auto_unstick_max_delete = 100;
};

struct LoggingConfig {
    bool         verbose = false;
    std::string  path;
};

struct SynthConfig {
    // Fabricate a synthetic ConZPlayerController + pawn so commands that
    // enumerate world controllers (e.g. listplayers) complete headless.
    // WARNING: spawning a player controller + pawn and possessing on a
    // dedicated server can crash it. Defaults to OFF.
    bool pc = false;
};

struct Config {
    RconConfig    rcon;
    QuestsConfig  quests;
    LoggingConfig logging;
    SynthConfig   synth;
};

// Loads config.ini from the mod's own directory. If missing, writes a default
// template and returns false (caller must abort startup).
bool load(Config& out);

// Global singleton configuration, filled by RconMod::on_unreal_init().
Config& current();

} // namespace scum_rcon::config
