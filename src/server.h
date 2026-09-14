// server.h -- Source RCON TCP server
#pragma once
#include "rcon_protocol.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace scum_rcon::server {

struct ListenerConfig {
    std::string  bind_address = "0.0.0.0";
    std::uint16_t port        = 25575;
    std::string  password;
    bool         auth_log     = true;
    std::size_t  analysis_limit   = 0;
    std::size_t  max_connections  = 16;
    std::size_t  max_auth_failures= 5;
};

// Blocks the calling thread until stop() is invoked.
void run(ListenerConfig const& cfg);
void stop();

} // namespace scum_rcon::server
