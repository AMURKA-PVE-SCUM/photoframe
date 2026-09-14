// engine_hooks.cpp -- engine-side hooks
//
// Uses RC::Unreal::UObjectGlobals::RegisterHook to capture the output that
// AdminCommand replies produce (chat lines and client result notifications)
// so dispatch_command() can return it to the RCON client, and to react to
// engine shutdown (GameInstance:ReceiveShutdown).
#include "engine_hooks.h"
#include "dispatch.h"
#include "server.h"
#include "scum_rcon.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace scum_rcon::engine_hooks {

namespace {

std::mutex  g_hook_mutex;
std::vector<std::pair<std::string, std::uint64_t>> g_registered_callbacks;

std::atomic<bool> g_shutdown_fired{false};

// Read the first FString parameter whose name is in `candidates` from a hook
// params buffer and append it to the dispatch capture buffer.
void capture_string_param(RC::Unreal::UnrealScriptFunctionCallableContext& ctx,
                          void* params, const wchar_t* const* candidates,
                          int n)
{
    RC::Unreal::UFunction* fn = ctx.TheStack.Node();
    if (!fn || !params) return;

    RC::Unreal::FProperty* prop = nullptr;
    int found = -1;
    for (int i = 0; i < n; ++i) {
        auto* p = fn->FindProperty(RC::Unreal::FName(
            candidates[i], RC::Unreal::EFindName::FName_Add));
        if (p) { prop = p; found = i; break; }
    }
    if (!prop) return;

    int off = prop->GetOffset_Internal();
    auto* fs = reinterpret_cast<RC::Unreal::FString*>(
        reinterpret_cast<char*>(params) + off);
    std::wstring ws = fs->wstring();
    if (ws.empty()) return;
    dispatch::capture_append(log::narrow(ws));
}

const wchar_t* const kMessageCandidates[] = { L"Message", L"Text", L"Str",
                                              L"Content", L"Result",
                                              L"MessageText", L"CommandResult",
                                              L"OutputString" };
const wchar_t* const kResultCandidates[]  = { L"Result", L"Message", L"Text",
                                              L"Str", L"Content", L"AdminResult",
                                              L"CommandResult", L"OutputString",
                                              L"ResultText", L"MessageText" };

// ---- Chat-line capture --------------------------------------------------
void on_send_chat_line_pre(RC::Unreal::UnrealScriptFunctionCallableContext& ctx,
                           void* params)
{
    capture_string_param(ctx, params, kMessageCandidates,
                         static_cast<int>(std::size(kMessageCandidates)));
}

// ---- Admin-command output capture ---------------------------------------
// SCUM ships the admin-command reply as client RPCs on the player's
// PlayerRpcChannel, not via ClientShowAdminCommandResult (which does not
// exist in this build). The server-side ProcessEvent of these client RPCs
// still passes through our pre-hooks, so we capture the reply strings there.
void on_admin_output_pre(RC::Unreal::UnrealScriptFunctionCallableContext& ctx,
                         void* params)
{
    capture_string_param(ctx, params, kResultCandidates,
                         static_cast<int>(std::size(kResultCandidates)));
}

// ---- Shutdown hook ------------------------------------------------------
void on_receive_shutdown_pre(RC::Unreal::UnrealScriptFunctionCallableContext& /*ctx*/,
                             void* /*params*/)
{
    if (g_shutdown_fired.exchange(true)) return;
    log::line(L"shutdown-hook: GameInstance:ReceiveShutdown fired - gating drain and stopping listener");
    server::stop();
}

void on_receive_shutdown_post(RC::Unreal::UnrealScriptFunctionCallableContext& /*ctx*/,
                              void* /*params*/)
{
}

// Register a hook on an object path, resolving the UFunction first. Calling
// RegisterHook(path-string) directly is unsafe on UE4SS 3.0.1: its path
// overload forwards an unresolvable lookup to RegisterHook(UFunction*) and
// dereferences the bogus pointer (AV at +0xD8). Resolving first lets us skip
// paths that do not exist on the dedicated server (e.g. ConZ client-only).
std::pair<int, int> try_register_hook(
    const wchar_t* path,
    std::function<void(RC::Unreal::UnrealScriptFunctionCallableContext&, void*)> pre,
    std::function<void(RC::Unreal::UnrealScriptFunctionCallableContext&, void*)> post)
{
    auto* fn =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(nullptr, path);
    if (!fn) return {0, 0};
    return RC::Unreal::UObjectGlobals::RegisterHook(fn, pre, post, nullptr);
}

} // anonymous namespace

// ===========================================================================
//  install_all
// ===========================================================================
HookStats install_all()
{
    HookStats stats;

    if (install_chat_line_capture()) ++stats.installed; else ++stats.pattern_misses;
    if (install_admin_output_capture()) ++stats.installed; else ++stats.pattern_misses;
    if (install_shutdown_hook()) ++stats.installed; else ++stats.pattern_misses;

    // Pattern-scan / detour hooks cannot be installed without the internal
    // engine signatures; they are reported as unavailable (non-fatal).
    log::line(L"engine hooks ready (" + std::to_wstring(stats.installed) +
              L" hook(s), " + std::to_wstring(stats.pattern_misses) +
              L" unavailable)");
    return stats;
}

// ===========================================================================
//  install_chat_line_capture
// ===========================================================================
bool install_chat_line_capture()
{
    auto pair = try_register_hook(L"/Script/SCUM.MiscStatics:SendChatLineToPlayer",
                                  on_send_chat_line_pre, nullptr);
    if (pair.first == 0 && pair.second == 0) {
        log::line(L"capture: chat-line function not resolvable - command replies may be empty");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_hook_mutex);
        g_registered_callbacks.emplace_back(
            "SendChatLineToPlayer",
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pair.first)) << 32)
                | static_cast<std::uint32_t>(pair.second));
    }
    log::line(L"capture: chat-line hook installed");
    return true;
}

// ===========================================================================
//  install_admin_output_capture
// ===========================================================================
bool install_admin_output_capture()
{
    // ClientShowAdminCommandResult/ClientShowCommandResult are client-only
    // RPCs that do not exist on this dedicated-server build. The reply
    // actually arrives through client RPCs on PlayerRpcChannel, so hook those
    // by their observed paths. (The old paths are kept harmlessly; they just
    // won't resolve.)
    constexpr wchar_t const* kTargets[14] = {
        // Observed in this build (/Script/SCUM.PlayerRpcChannel:...):
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_SendMessageToChat",
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_ProcessAdminCommand",
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_MuteUser",
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_UnmuteUser",
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_UpdatePlayerArgumentCompletionCache",
        L"/Script/SCUM.PlayerRpcChannel:Chat_Client_UpdateSquadArgumentCompletionCache",
        L"/Script/SCUM.PlayerRpcChannel:Admin_Client_ShowSpawnedVehiclesPartial",
        L"/Script/SCUM.PlayerRpcChannel:Admin_Client_SendRespawnTimes",
        L"/Script/SCUM.PlayerRpcChannel:Client_DialPadAttemptResult",
        L"/Script/SCUM.PlayerRpcChannel:RaidProtection_Client_ShowPlayerLoginMessages",
        // Historical paths (client-only in older builds; harmless if missing):
        L"/Script/SCUM.ConZPlayerController:ClientShowAdminCommandResult",
        L"/Script/ConZ.ConZPlayerController:ClientShowAdminCommandResult",
        L"/Script/SCUM.ConZPlayerController:ClientShowCommandResult",
        L"/Script/ConZ.ConZPlayerController:ClientShowCommandResult",
    };
    int installed = 0;
    for (auto* path : kTargets) {
        auto pair = try_register_hook(path, on_admin_output_pre, nullptr);
        if (pair.first != 0 || pair.second != 0) {
            ++installed;
            std::lock_guard<std::mutex> lk(g_hook_mutex);
            g_registered_callbacks.emplace_back(
                "AdminOutputCapture",
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pair.first)) << 32)
                    | static_cast<std::uint32_t>(pair.second));
            log::line(std::wstring(L"capture: admin-output hook installed on ") + path);
        }
    }
    if (installed == 0) {
        log::line(L"capture: admin-output hooks unavailable - list-command replies will be empty");
        return false;
    }
    log::line(L"capture: admin-output hooks installed (" + std::to_wstring(installed) +
              L"/" + std::to_wstring(std::size(kTargets)) + L") - list-command replies now captured");
    return true;
}

// ===========================================================================
//  install_shutdown_hook
// ===========================================================================
bool install_shutdown_hook()
{
    auto pair = try_register_hook(L"/Script/Engine.GameInstance:ReceiveShutdown",
                                  on_receive_shutdown_pre, on_receive_shutdown_post);
    if (pair.first == 0 && pair.second == 0) {
        log::line(L"shutdown-hook: GameInstance:ReceiveShutdown not hookable (empty BP event)");
        return false;
    }
    log::line(L"shutdown-hook installed on GameInstance:ReceiveShutdown");
    return true;
}

} // namespace scum_rcon::engine_hooks