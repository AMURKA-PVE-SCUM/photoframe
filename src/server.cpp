// server.cpp -- Source RCON TCP server implementation
//
// Winsock listener implementing the Valve Source RCON protocol:
//   AUTH (3) -> AUTH_RESPONSE (2), EXECCOMMAND (2) -> RESPONSE_VALUE (0).
// Each authenticated command is enqueued onto the game-thread drain
// (game_thread::enqueue) which returns the reply string.
#include "server.h"
#include "game_thread.h"
#include "scum_rcon.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <sstream>
#include <thread>

namespace scum_rcon::server {

namespace {

struct ListenerState {
    ListenerConfig            cfg;
    SOCKET                    listen_sock = INVALID_SOCKET;
    std::atomic<bool>         stopping{false};
    std::mutex                conns_mutex;
    std::vector<SOCKET>       active_conns;
    std::mutex                fails_mutex;
    std::unordered_map<std::string, std::size_t> auth_failures;
};

ListenerState g_state;

void log_info(std::wstring_view msg) { log::line(msg); }
void log_warn(std::wstring_view msg) { log::line(msg); }
void log_error(std::wstring_view msg) { log::line(msg); }

std::wstring wformat(std::wstring_view a, std::wstring_view b)
{
    std::wstring out(a);
    out.append(b);
    return out;
}

// ---- Send a complete RCON packet ----------------------------------------
// Valve spec: Size = ID(4) + Type(4) + Body + 2 NUL terminators. Some
// strict clients desync/timeout if the second NUL is missing.
bool send_packet(SOCKET s, std::int32_t id, rcon::PacketType type,
                 std::string const& body)
{
    std::int32_t size = static_cast<std::int32_t>(4 + 4 + body.size() + 2);
    if (::send(s, reinterpret_cast<char const*>(&size), 4, 0) != 4) return false;
    if (::send(s, reinterpret_cast<char const*>(&id),   4, 0) != 4) return false;
    std::int32_t t = static_cast<std::int32_t>(type);
    if (::send(s, reinterpret_cast<char const*>(&t),    4, 0) != 4) return false;
    if (!body.empty() &&
        ::send(s, body.data(), static_cast<int>(body.size()), 0)
         != static_cast<int>(body.size())) return false;
    char zero[2] = {0, 0};
    return ::send(s, zero, 2, 0) == 2;
}

// ---- Recv exactly N bytes (blocking) ------------------------------------
bool recv_exact(SOCKET s, void* buf, std::size_t n)
{
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        int got = ::recv(s, p, static_cast<int>(n), 0);
        if (got <= 0) return false;
        p += got;
        n -= static_cast<std::size_t>(got);
    }
    return true;
}

// ---- Per-connection handler ---------------------------------------------
void handle_connection(SOCKET sock, std::string peer_addr)
{
    rcon::ConnState state = rcon::ConnState::AwaitingAuth;

    while (!g_state.stopping) {
        std::int32_t size = 0;
        if (!recv_exact(sock, &size, 4)) break;

        if (size < static_cast<std::int32_t>(rcon::MIN_PACKET_SIZE)
            || static_cast<std::uint32_t>(size) > rcon::MAX_PACKET_SIZE) {
            log_warn(L"rcon: client sent oversized data without a valid packet - dropping");
            break;
        }

        std::vector<char> buf(static_cast<std::size_t>(size));
        if (!recv_exact(sock, buf.data(), buf.size())) {
            log_warn(L"rcon: client sent a malformed packet - dropping");
            break;
        }

        std::int32_t id   = *reinterpret_cast<std::int32_t*>(&buf[0]);
        std::int32_t type = *reinterpret_cast<std::int32_t*>(&buf[4]);
        std::string  body(buf.begin() + 8, buf.end());
        // RCON packets terminate the body with a double NUL ("password\0\0");
        // strip every trailing NUL so comparisons see exactly the payload.
        while (!body.empty() && body.back() == '\0') body.pop_back();

        switch (state) {
        case rcon::ConnState::AwaitingAuth: {
            if (type != rcon::SERVERDATA_AUTH) {
                log_warn(L"rcon: client sent a command before authenticating - dropping");
                goto connection_end;
            }
            bool ok = (body == g_state.cfg.password);
            log_info(ok ? L"rcon: auth OK from peer" : L"rcon: auth FAIL from peer");
            if (!ok) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(rcon::AUTH_PENALTY_MS));

                bool locked = false;
                {
                    std::lock_guard<std::mutex> lk(g_state.fails_mutex);
                    if (++g_state.auth_failures[peer_addr]
                            >= g_state.cfg.max_auth_failures) {
                        locked = true;
                    }
                }
                if (locked) {
                    log_warn(L"rcon: peer locked out after repeated auth failures");
                }
                send_packet(sock, rcon::AUTH_FAILURE_ID,
                            rcon::PacketType::AuthResponse, "");
                goto connection_end;
            }
            {
                std::lock_guard<std::mutex> lk(g_state.fails_mutex);
                g_state.auth_failures.erase(peer_addr);
            }
            send_packet(sock, id, rcon::PacketType::ResponseValue, "");
            send_packet(sock, id, rcon::PacketType::AuthResponse, "");
            state = rcon::ConnState::Authenticated;
            break;
        }
        case rcon::ConnState::Authenticated: {
            if (type != rcon::SERVERDATA_EXECCOMMAND) {
                break; // ignore unexpected packets
            }
            std::string reply;
            std::mutex  m;
            std::condition_variable cv;
            bool done = false;
            {
                std::string preview = body.size() > 120
                    ? body.substr(0, 120) + "..." : body;
                log_info(wformat(L"rcon: cmd ", log::widen(preview)));
            }
            game_thread::enqueue(body, [&](std::string r) {
                std::lock_guard<std::mutex> lk(m);
                reply = std::move(r);
                done = true;
                cv.notify_one();
            });
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return done; });

            if (reply.size() <= rcon::REPLY_CHUNK_SIZE) {
                send_packet(sock, id, rcon::PacketType::ResponseValue, reply);
            } else {
                std::size_t off = 0;
                while (off < reply.size()) {
                    std::size_t n = std::min(rcon::REPLY_CHUNK_SIZE,
                                             reply.size() - off);
                    send_packet(sock, id, rcon::PacketType::ResponseValue,
                                reply.substr(off, n));
                    off += n;
                }
            }
            send_packet(sock, id, rcon::PacketType::ResponseValue, "");
            break;
        }
        case rcon::ConnState::Closing:
            goto connection_end;
        }
    }

connection_end:
    {
        std::lock_guard<std::mutex> lk(g_state.conns_mutex);
        g_state.active_conns.erase(
            std::remove(g_state.active_conns.begin(),
                        g_state.active_conns.end(), sock),
            g_state.active_conns.end());
    }
    ::shutdown(sock, SD_BOTH);
    ::closesocket(sock);
}

} // anonymous namespace

// ===========================================================================
//  run() -- main listener loop
// ===========================================================================
void run(ListenerConfig const& cfg)
{
    g_state.cfg = cfg;

    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_error(L"RCON listener NOT started: WSAStartup failed");
        return;
    }

    g_state.listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_state.listen_sock == INVALID_SOCKET) {
        log_error(L"RCON listener NOT started: socket() failed");
        return;
    }

    BOOL opt = TRUE;
    ::setsockopt(g_state.listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<char const*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(cfg.port);
    ::inet_pton(AF_INET, cfg.bind_address.c_str(), &addr.sin_addr);
    if (::bind(g_state.listen_sock, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == SOCKET_ERROR) {
        log_error(wformat(L"rcon: bind failed (port in use?) - ",
                          log::widen(cfg.bind_address)));
        ::closesocket(g_state.listen_sock);
        return;
    }

    if (::listen(g_state.listen_sock, 8) == SOCKET_ERROR) {
        log_error(L"RCON listener NOT started: listen() failed");
        ::closesocket(g_state.listen_sock);
        return;
    }

    log_info(wformat(L"SCUM-RCON ready - listening on ",
                     log::widen(cfg.bind_address)));
    log_info(L"  mcrcon -H 127.0.0.1 -P " + std::to_wstring(cfg.port) +
             L" -p <your-password> -t");
    log_info(L"Then type any admin command, e.g.:  ListSquads");

    while (!g_state.stopping) {
        sockaddr_in client{};
        int clen = sizeof(client);
        SOCKET sock = ::accept(g_state.listen_sock,
                               reinterpret_cast<sockaddr*>(&client), &clen);
        if (sock == INVALID_SOCKET) {
            if (g_state.stopping) break;
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        bool refused = false;
        {
            std::lock_guard<std::mutex> lk(g_state.conns_mutex);
            if (g_state.active_conns.size() >= g_state.cfg.max_connections) {
                refused = true;
            } else {
                g_state.active_conns.push_back(sock);
            }
        }
        if (refused) {
            log_warn(L"rcon: connection limit reached - refusing new connection");
            ::closesocket(sock);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(g_state.fails_mutex);
            auto it = g_state.auth_failures.find(ip);
            if (it != g_state.auth_failures.end()
                && it->second >= g_state.cfg.max_auth_failures) {
                log_warn(L"rcon: peer is locked out - refusing connection");
                ::closesocket(sock);
                continue;
            }
        }

        std::thread(handle_connection, sock, std::string(ip)).detach();
    }

    log_info(L"rcon: stopping listener...");
    {
        std::lock_guard<std::mutex> lk(g_state.conns_mutex);
        for (SOCKET s : g_state.active_conns) {
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
        }
        g_state.active_conns.clear();
    }
    ::closesocket(g_state.listen_sock);
    ::WSACleanup();
}

void stop()
{
    g_state.stopping.store(true);
    if (g_state.listen_sock != INVALID_SOCKET) {
        ::shutdown(g_state.listen_sock, SD_BOTH);
        ::closesocket(g_state.listen_sock);
        g_state.listen_sock = INVALID_SOCKET;
    }
}

} // namespace scum_rcon::server
