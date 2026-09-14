// rcon_protocol.h -- Valve Source RCON protocol constants
#pragma once
#include <cstdint>
#include <string>

namespace scum_rcon::rcon {

// Packet type IDs (Valve Source RCON spec)
enum class PacketType : std::int32_t {
    ResponseValue = 0,   // SERVERDATA_RESPONSE_VALUE
    AuthResponse  = 2,   // SERVERDATA_AUTH_RESPONSE
    Execute       = 2,   // SERVERDATA_EXECCOMMAND (same numeric value, disambiguated by direction)
    Auth          = 3,   // SERVERDATA_AUTH
};

constexpr std::uint32_t MIN_PACKET_SIZE = 10;
constexpr std::uint32_t MAX_PACKET_SIZE = 4096;
constexpr std::uint32_t MAX_BODY_SIZE   = MAX_PACKET_SIZE - 12;

constexpr std::uint32_t SERVERDATA_AUTH           = 3;
constexpr std::uint32_t SERVERDATA_AUTH_RESPONSE  = 2;
constexpr std::uint32_t SERVERDATA_EXECCOMMAND    = 2;
constexpr std::uint32_t SERVERDATA_RESPONSE_VALUE = 0;

constexpr std::int32_t  AUTH_FAILURE_ID   = -1;
constexpr std::size_t   MAX_AUTH_FAILURES = 5;
constexpr std::size_t   AUTH_PENALTY_MS   = 1000;

#pragma pack(push, 1)
struct PacketHeader {
    std::int32_t size;   // length of payload (id + type + body + null), not incl. itself
    std::int32_t id;     // client-chosen request ID, echoed back by server
    std::int32_t type;   // PacketType
    // followed by: char body[]; char null_terminator;
};
#pragma pack(pop)

// Per-connection state machine.
enum class ConnState {
    AwaitingAuth,
    Authenticated,
    Closing,
};

// Long replies are split into multiple RESPONSE_VALUE packets.
constexpr std::size_t REPLY_CHUNK_SIZE = 4000;

} // namespace scum_rcon::rcon
