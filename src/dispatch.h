// dispatch.h -- in-game command dispatch pipeline
#pragma once
#include "scum_rcon.h"

#include <string>
#include <vector>

// Completes the forward declaration in UE4SSStubs/UE4SS.h. Layout confirmed
// for UE >= 4.26 (0x40 bytes):
//   0x00 UClass*          Class
//   0x08 UObject*         Outer          (nullptr => transient package)
//   0x10 FName            Name
//   0x18 EObjectFlags     SetFlags
//   0x1c EInternalObjectFlags InternalSetFlags
//   0x20 bool             bCopyTransientsFromClassDefaults
//   0x21 bool             bAssumeTemplateIsArchetype
//   0x28 UObject*         Template       (nullptr => class default object)
//   0x30 FObjectInstancingGraph* InstanceGraph
//   0x38 UPackage*        ExternalPackage
namespace RC::Unreal {
struct FStaticConstructObjectParameters {
    UClass*          Class;
    UObject*         Outer;
    FName            Name;
    std::uint32_t    SetFlags;
    std::uint32_t    InternalSetFlags;
    bool             bCopyTransientsFromClassDefaults;
    bool             bAssumeTemplateIsArchetype;
    UObject*         Template;
    void*            InstanceGraph;
    void*            ExternalPackage;
};
} // namespace RC::Unreal

static_assert(sizeof(RC::Unreal::FStaticConstructObjectParameters) == 0x40,
              "FStaticConstructObjectParameters layout must be 0x40 bytes on UE4.27");

namespace scum_rcon::dispatch {

struct Result {
    bool        ok     = false;
    std::string reply;
    std::size_t lines  = 0;
};

// Build the verb -> UClass/UFunction map by scanning the UClass tree for
// subclasses of /Script/SCUM.AdminCommand. Returns the number of verbs found.
std::uint64_t build_verb_map();

// Resolve the native SCUM command-executor function by byte-signature in the
// process's primary image (.text). Returns the function address, or 0 if the
// signature is not present (e.g. a different SCUM build).
std::uintptr_t resolve_native_executor();

// Log every live UFunction whose name looks like an admin-result sink, so
// the output-capture hook targets can be reconciled with this SCUM build.
void probe_admin_result_functions();

// Find an online player controller to use as a command caller.
RC::Unreal::UObject* find_first_online_controller();

// Find a specific player's controller by 17-digit SteamID (best-effort).
RC::Unreal::UObject* find_controller_by_steam_id(std::string const& steam_id);

// SendChat custom command. Args: [type 0-7, "message", optional 17-digit SteamID].
Result dispatch_sendchat(std::vector<std::string> const& args);

// Top-level entry called by the game-thread drain.
std::string dispatch_command(std::string const& cmdline);

// Capture buffer written by the engine_hooks output-capture hooks.
void capture_append(std::string line);
void capture_clear();
std::vector<std::string> capture_snapshot();

} // namespace scum_rcon::dispatch
