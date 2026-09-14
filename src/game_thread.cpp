// game_thread.cpp -- engine-tick command drain
//
// The RCON listener thread cannot touch the game thread directly, so every
// command is pushed onto a queue and executed on the game thread. The drain
// runs from the EngineTick pre-callback (installed below) and additionally
// from RconMod::on_update, whichever fires first.
#include "game_thread.h"
#include "dispatch.h"
#include "scum_rcon.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace scum_rcon::game_thread {

namespace {

struct Job {
    std::string                   cmdline;
    std::function<void(std::string)> on_complete;
};

struct Queue {
    std::mutex              m;
    std::condition_variable cv;
    std::deque<Job>         jobs;
    std::atomic<bool>       dispatching{false};
};
Queue g_queue;

bool g_drain_ready = false;

void drain_one()
{
    Job job;
    {
        std::lock_guard<std::mutex> lk(g_queue.m);
        if (g_queue.jobs.empty()) return;
        job = std::move(g_queue.jobs.front());
        g_queue.jobs.pop_front();
        g_queue.dispatching.store(true);
    }

    std::string reply = dispatch::dispatch_command(job.cmdline);
    if (job.on_complete) job.on_complete(reply);

    g_queue.dispatching.store(false);
    g_queue.cv.notify_all();
}

// ---- EngineTick pre-callback (primary path) -----------------------------
void on_engine_tick_pre(RC::Unreal::Hook::TCallbackIterationData<void>& /*iter*/,
                        RC::Unreal::UEngine* /*engine*/,
                        float /*delta_seconds*/,
                        bool /*game_thread*/)
{
    drain_one();
}

// ---- ProcessEvent pre-callback (fallback path) --------------------------
void on_process_event_pre(RC::Unreal::Hook::TCallbackIterationData<void>& /*iter*/,
                          RC::Unreal::UObject* /*ctx*/,
                          RC::Unreal::UFunction* /*fn*/,
                          void* /*params*/)
{
    drain_one();
}

} // anonymous namespace

// ===========================================================================
//  install_tick_drain
// ===========================================================================
bool install_tick_drain()
{
    if (RC::UE4SSRuntime::IsEngineTickAvailable()) {
        RC::Unreal::Hook::FCallbackOptions opts;
        RC::Unreal::Hook::RegisterEngineTickPreCallback(on_engine_tick_pre, opts);
        g_drain_ready = true;
        log::line(L"game-thread drain installed via EngineTick pre-callback");
        return true;
    }

    if (RC::UE4SSRuntime::IsProcessEventAvailable()) {
        RC::Unreal::Hook::FCallbackOptions opts;
        RC::Unreal::Hook::RegisterProcessEventPreCallback(on_process_event_pre, opts);
        g_drain_ready = true;
        log::line(L"game-thread drain on FALLBACK path (ProcessEvent) - set 'HookEngineTick = 1' in UE4SS-settings.ini for the clean-frame path");
        return true;
    }

    log::line(L"game-thread drain: could not install an EngineTick or ProcessEvent hook - RCON commands will NOT execute. Set 'HookEngineTick = 1' in the [Hooks] section of UE4SS-settings.ini and restart the server.");
    return false;
}

// ===========================================================================
//  enqueue -- called from the listener thread
// ===========================================================================
void enqueue(std::string cmdline, std::function<void(std::string)> on_complete)
{
    {
        std::lock_guard<std::mutex> lk(g_queue.m);
        g_queue.jobs.push_back({ std::move(cmdline), std::move(on_complete) });
    }
    g_queue.cv.notify_one();
}

// ===========================================================================
//  tick -- drain at most one queued command (called from on_update)
// ===========================================================================
void tick()
{
    if (!g_drain_ready) return;
    drain_one();
}

} // namespace scum_rcon::game_thread
