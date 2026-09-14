// RconMod.cpp -- main mod entry points
//
// on_unreal_init() performs the full startup sequence:
//   1. env-scan for inline-hooking AV/EDR modules
//   2. load config.ini (write default template if missing)
//   3. fail closed if the password is unset / still the default
//   4. install engine hooks (output capture, shutdown hook)
//   5. build the verb map (AdminCommand subclasses)
//   6. install the game-thread command drain
//   7. run the boot auto-unstick (if configured)
//   8. start the RCON listener thread
//
// on_update() drains the RCON command queue on the game thread (fallback to
// the EngineTick pre-callback installed in step 6).
#include "RconMod.h"
#include "config.h"
#include "dispatch.h"
#include "engine_hooks.h"
#include "env_scan.h"
#include "game_thread.h"
#include "questdb.h"
#include "server.h"
#include "scum_rcon.h"

#include <string>
#include <thread>

namespace scum_rcon {

RconMod::RconMod()
{
    ModName = L"SCUM-RCON";
    ModVersion = L"1.0.0";
    ModDescription = L"Standalone Source RCON server for SCUM private servers";
    ModAuthors = L"SCUM-RCON port";
    ModIntendedSDKVersion = L"";
}

RconMod::~RconMod()
{
    server::stop();
}

void RconMod::on_unreal_init()
{
    // (1) env-scan
    auto av = env_scan::scan();
    if (!av.empty()) {
        log::line(L"env-scan: inline-hooking AV/EDR module(s) detected in the server process - exclude ScumServer.exe and the ue4ss folder from real-time / behaviour monitoring to avoid instability");
    }

    // (2) config
    if (!config::load(config::current())) {
        log::line(L"startup aborted: set a real password in config.ini and restart");
        return;
    }
    if (config::current().rcon.password.empty()
        || config::current().rcon.password == "CHANGE_ME_BEFORE_USE") {
        log::line(L"password is unset or still the default - set a real password in config.ini and restart");
        return;
    }

    // (4) engine hooks
    auto hook_stats = engine_hooks::install_all();

    // (5) verb map
    dispatch::build_verb_map();
    dispatch::probe_admin_result_functions();
    dispatch::resolve_native_executor();

    // (6) game-thread drain
    game_thread::install_tick_drain();

    // (7) boot auto-unstick
    if (config::current().quests.auto_unstick) {
        auto r = questdb::run_quest_unstick(
            config::current().quests.blocked,
            config::current().quests.auto_unstick_max_delete,
            config::current().quests.auto_unstick_dry_run);
        log::line(L"auto-unstick (boot): " + std::to_wstring(r.matches) +
                  L" match(es), " + std::to_wstring(r.deleted) + L" deleted" +
                  (r.aborted ? L" (ABORTED - circuit breaker)" : L"") +
                  (config::current().quests.auto_unstick_dry_run ? L" (dry-run)" : L""));
    }

    // (8) RCON listener thread
    server::ListenerConfig lcfg;
    lcfg.bind_address   = config::current().rcon.bind_address;
    lcfg.port           = config::current().rcon.port;
    lcfg.password       = config::current().rcon.password;
    lcfg.auth_log       = config::current().rcon.auth_log;
    lcfg.analysis_limit = config::current().rcon.analysis_limit;
    std::thread([lcfg]() { server::run(lcfg); }).detach();

    log::line(L"SCUM-RCON READY - server up (tick loop live); commands enabled");
}

void RconMod::on_update()
{
    game_thread::tick();
}

} // namespace scum_rcon