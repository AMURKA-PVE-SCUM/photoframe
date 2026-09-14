// dispatch.cpp -- command dispatch pipeline
//
// Routes an RCON command line to either:
//   * a custom C++ verb (SendChat / ListSquads / FindQuestLockouts /
//     RunQuestUnstick / DeleteActiveQuestsForUser / Unstuck), or
//   * an AdminCommand subclass found in the UClass tree (verb map).
//
// Invocation uses UObject::ProcessEvent on the command's ClassDefaultObject
// with a params buffer sized by UFunction::GetParmsSize(); string params are
// placed as game FStrings and destroyed via FProperty::DestroyValue after the
// call (the pattern UE4SS itself uses).
#include "dispatch.h"
#include "config.h"
#include "questdb.h"
#include "synth.h"
#include "scum_rcon.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace scum_rcon::dispatch {

// Engine-accurate EObjectFlags for transient scratch command instances. The
// stub's EObjectFlags omits intermediate bits (there RF_Transient is 0x08,
// which is really RF_Transactional; UE's true RF_Transient is 0x40).
constexpr std::uint32_t SCO_RF_NoFlags   = 0x00000000;
constexpr std::uint32_t SCO_RF_Transient = 0x00000040;

namespace {

// ---- Verb map -----------------------------------------------------------
struct VerbEntry {
    RC::Unreal::UClass*    klass = nullptr;
    RC::Unreal::UFunction* fn    = nullptr;  // optional; null => chat-pipeline verb
};

std::unordered_map<std::string, VerbEntry> g_verb_map;
std::mutex                                  g_verb_mutex;
bool                                        g_verb_map_built = false;
std::uint64_t                               g_verb_count     = 0;

// ---- Capture buffer -----------------------------------------------------
std::mutex               g_capture_mutex;
std::vector<std::string> g_capture_lines;

// Walk a UStruct's child UFields (functions) and log their names, to
// discover how an AdminCommand subclass exposes its execute entry point.
void log_struct_fields(std::wstring const& label, RC::Unreal::UStruct* s)
{
    if (!s) return;
    std::wstring out = label + L": ";
    RC::Unreal::UField* f = s->GetChildren().ObjectPtr;
    int n = 0;
    while (f && n < 40) {
        out += f->GetName();
        out += L"  ";
        f = f->GetNext().ObjectPtr;
        ++n;
    }
    if (n == 0) out += L"(no children)";
    log::line(out);
}

// ---- Params-buffer helpers ---------------------------------------------
struct StringParam {
    RC::Unreal::FProperty* prop = nullptr;
    int offset = 0;
};
std::vector<StringParam> g_set_strings;

// Walk a UFunction's parameter properties and log every name + offset by
// traversing the ChildProperties (FField) chain. We cannot read the value
// types from the stub, but names + offsets let us place the command string
// directly.
void log_function_params(std::wstring const& label, RC::Unreal::UFunction* fn)
{
    if (!fn) { log::line(label + L": (null)"); return; }
    std::wstring out = label + L" (parmsize=" +
        std::to_wstring(fn->GetParmsSize()) + L", numparms=" +
        std::to_wstring(static_cast<int>(fn->GetNumParms())) + L"): ";
    RC::Unreal::FField* f = fn->GetChildProperties();
    int n = 0;
    while (f && n < 24) {
        auto* prop = static_cast<RC::Unreal::FProperty*>(f);
        out += f->GetName();
        out += L"@";
        out += std::to_wstring(prop->GetOffset_Internal());
        out += L"  ";
        f = f->NextField();
        ++n;
    }
    if (n == 0) out += L"(no child properties)";
    log::line(out);
}

// Set a string into the first non-return parameter of `fn`, wherever it is.
// Returns the offset used, or -1 if fn has no usable string-sized slot.
int set_first_string_param(RC::Unreal::UFunction* fn, void* buf,
                           std::wstring const& value)
{
    if (!buf || !fn) return -1;
    RC::Unreal::FField* f = fn->GetChildProperties();
    while (f) {
        auto* prop = static_cast<RC::Unreal::FProperty*>(f);
        if (!prop->HasAllPropertyFlags(RC::Unreal::EPropertyFlags::CPF_ReturnParm)) {
            int off = prop->GetOffset_Internal();
            int size = prop->GetSize();
            // FString is 16 bytes in UE4.27. Only place into a matching slot.
            if (size == 16) {
                new (reinterpret_cast<char*>(buf) + off)
                    RC::Unreal::FString(value);
                g_set_strings.push_back(StringParam{ prop, off });
                return off;
            }
        }
        f = f->NextField();
    }
    return -1;
}

// Try a list of candidate UFunction names on an object's class chain and
// report the first hit.
RC::Unreal::UFunction* probe_execute_fn(RC::Unreal::UStruct* s)
{
    if (!s) return nullptr;
    static const wchar_t* const kCandidates[] = {
        L"Execute",  L"ExecuteCommand", L"RunCommand", L"Command",
        L"DoWork",   L"ExecuteWithCommandArgs", L"Action",
    };
    for (auto* name : kCandidates) {
        RC::Unreal::UFunction* fn =
            s->GetFunctionByNameInChain(name);
        if (fn) {
            log::line(std::wstring(L"probe: found func '") + name +
                      L"' on " + s->GetName());
            return fn;
        }
    }
    return nullptr;
}

// ---- Argument splitting -------------------------------------------------
std::vector<std::string> split_args(std::string const& cmdline)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (char c : cmdline) {
        if (c == '"') { in_quote = !in_quote; continue; }
        if (std::isspace(static_cast<unsigned char>(c)) && !in_quote) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// Find a property, walking the super-struct chain (FindProperty is
// class-local in UE).
RC::Unreal::FProperty* find_property_in_chain(RC::Unreal::UStruct* s,
                                              RC::Unreal::FName name)
{
    while (s) {
        if (RC::Unreal::FProperty* p = s->FindProperty(name)) return p;
        s = s->GetSuperStruct();
    }
    return nullptr;
}

// ---- Params-buffer helpers ---------------------------------------------
void* make_params(RC::Unreal::UFunction* fn)
{
    std::uint16_t size = fn->GetParmsSize();
    if (size == 0) return nullptr;
    void* buf = ::operator new(size);
    std::memset(buf, 0, size);
    return buf;
}

bool set_string_param(RC::Unreal::UFunction* fn, void* buf,
                      const wchar_t* name, std::wstring const& value)
{
    if (!buf) return false;
    RC::Unreal::FProperty* prop =
        fn->FindProperty(RC::Unreal::FName(name, RC::Unreal::EFindName::FName_Add));
    if (!prop) return false;
    int off = prop->GetOffset_Internal();
    void* dst = reinterpret_cast<char*>(buf) + off;
    new (dst) RC::Unreal::FString(value);
    g_set_strings.push_back(StringParam{ prop, off });
    return true;
}

bool set_int_param(RC::Unreal::UFunction* fn, void* buf,
                   const wchar_t* name, int value)
{
    if (!buf) return false;
    RC::Unreal::FProperty* prop =
        fn->FindProperty(RC::Unreal::FName(name, RC::Unreal::EFindName::FName_Add));
    if (!prop) return false;
    int off = prop->GetOffset_Internal();
    std::memcpy(reinterpret_cast<char*>(buf) + off, &value, sizeof(int));
    return true;
}

bool set_object_param(RC::Unreal::UFunction* fn, void* buf,
                      const wchar_t* name, RC::Unreal::UObject* value)
{
    if (!buf) return false;
    RC::Unreal::FProperty* prop =
        fn->FindProperty(RC::Unreal::FName(name, RC::Unreal::EFindName::FName_Add));
    if (!prop) return false;
    int off = prop->GetOffset_Internal();
    std::memcpy(reinterpret_cast<char*>(buf) + off, &value, sizeof(value));
    return true;
}

void cleanup_params(void* buf)
{
    if (!buf) return;
    for (auto const& sp : g_set_strings) {
        sp.prop->DestroyValue(reinterpret_cast<char*>(buf) + sp.offset);
    }
    g_set_strings.clear();
    ::operator delete(buf);
}

// Invoke a UFunction on an object with an optional string/object param set.
void call_ufunction(RC::Unreal::UObject* self, RC::Unreal::UFunction* fn)
{
    if (!self || !fn) return;
    void* buf = make_params(fn);
    self->ProcessEvent(fn, buf);
    cleanup_params(buf);
}

void call_ufunction_string(RC::Unreal::UObject* self, RC::Unreal::UFunction* fn,
                           const wchar_t* arg_name, std::wstring const& value)
{
    if (!self || !fn) return;
    void* buf = make_params(fn);
    set_string_param(fn, buf, arg_name, value);
    self->ProcessEvent(fn, buf);
    cleanup_params(buf);
}

// Read the FString return value of a UFunction after ProcessEvent. Returns
// false if the function has no return slot.
bool read_fstring_return(RC::Unreal::UFunction* fn, void* buf, std::wstring& out)
{
    std::uint16_t ret_off = fn->GetReturnValueOffset();
    if (ret_off == 0xFFFF) return false;
    auto* fs = reinterpret_cast<RC::Unreal::FString*>(
        reinterpret_cast<char*>(buf) + ret_off);
    out = fs->wstring();
    // Free the returned string buffer via its ReturnValue property.
    RC::Unreal::FProperty* rp = fn->FindProperty(
        RC::Unreal::FName(L"ReturnValue", RC::Unreal::EFindName::FName_Add));
    if (rp) rp->DestroyValue(fs);
    return true;
}

// ---- Controller enumeration --------------------------------------------
// PlayerController class, resolved once ("/Script/Engine.PlayerController").
RC::Unreal::UClass* g_player_controller_class = nullptr;

RC::Unreal::UClass* get_player_controller_class()
{
    if (g_player_controller_class) return g_player_controller_class;
    g_player_controller_class = RC::Unreal::UObjectGlobals::FindObject<
        RC::Unreal::UClass>(nullptr, L"/Script/Engine.PlayerController");
    return g_player_controller_class;
}

// GetUserId on a controller via ProcessEvent; best-effort.
std::string controller_steam_id(RC::Unreal::UObject* pc)
{
    RC::Unreal::UFunction* fn = pc->GetFunctionByNameInChain(L"GetUserId");
    if (!fn) return {};
    void* buf = make_params(fn);
    pc->ProcessEvent(fn, buf);
    std::wstring ws;
    read_fstring_return(fn, buf, ws);
    cleanup_params(buf);
    std::string result = log::narrow(ws);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0' || result.back() == '\r' || result.back() == '\n'))
        result.pop_back();
    return result;
}

// Resolve an object-typed property (PlayerState / Pawn) from an object.
RC::Unreal::UObject* resolve_object_prop(RC::Unreal::UObject* obj,
                                         const wchar_t* prop_name)
{
    if (!obj || !obj->GetClassPrivate()) return nullptr;
    RC::Unreal::FProperty* p = find_property_in_chain(
        obj->GetClassPrivate(),
        RC::Unreal::FName(prop_name, RC::Unreal::EFindName::FName_Add));
    if (!p) return nullptr;
    RC::Unreal::UObject* out = nullptr;
    std::memcpy(&out, reinterpret_cast<char*>(obj) + p->GetOffset_Internal(),
                sizeof(out));
    return out;
}

// Call a no-arg UFunction with an FString return value.
bool call_fstring_fn(RC::Unreal::UObject* obj, const wchar_t* fname,
                     std::wstring& out)
{
    if (!obj) return false;
    RC::Unreal::UFunction* fn = obj->GetFunctionByNameInChain(fname);
    if (!fn) return false;
    std::uint16_t parm_size = fn->GetParmsSize();
    void* buf = parm_size ? ::operator new(parm_size) : nullptr;
    if (!buf) return false;
    std::memset(buf, 0, parm_size);
    obj->ProcessEvent(fn, buf);
    bool ok = read_fstring_return(fn, buf, out) && !out.empty();
    ::operator delete(buf);
    return ok;
}

// Call a no-arg UFunction with a 32-bit int return value.
bool call_int_fn(RC::Unreal::UObject* obj, const wchar_t* fname, int& out)
{
    if (!obj) return false;
    RC::Unreal::UFunction* fn = obj->GetFunctionByNameInChain(fname);
    if (!fn || fn->GetParmsSize() < 4) return false;
    void* buf = ::operator new(fn->GetParmsSize());
    std::memset(buf, 0, fn->GetParmsSize());
    obj->ProcessEvent(fn, buf);
    std::uint16_t ret_off = fn->GetReturnValueOffset();
    bool ok = false;
    if (ret_off != 0xFFFF) {
        std::int32_t v = 0;
        std::memcpy(&v, reinterpret_cast<char*>(buf) + ret_off, sizeof(v));
        out = static_cast<int>(v);
        ok = true;
    }
    ::operator delete(buf);
    return ok;
}

// Call a no-arg UFunction returning FVector (12 bytes = 3 floats).
struct Vec3f { float x = 0, y = 0, z = 0; };
bool call_fvector_fn(RC::Unreal::UObject* obj, const wchar_t* fname, Vec3f& out)
{
    if (!obj) return false;
    RC::Unreal::UFunction* fn = obj->GetFunctionByNameInChain(fname);
    if (!fn || fn->GetParmsSize() != 12) return false;
    void* buf = ::operator new(12);
    std::memset(buf, 0, 12);
    obj->ProcessEvent(fn, buf);
    std::uint16_t ret_off = fn->GetReturnValueOffset();
    bool ok = false;
    if (ret_off != 0xFFFF) {
        std::memcpy(&out, reinterpret_cast<char*>(buf) + ret_off, sizeof(out));
        ok = true;
    }
    ::operator delete(buf);
    return ok;
}

// Native ListPlayers in the ORIGINAL engine reply format:
//
//  1. <CharName>
// Steam: <SteamName> (<SteamID>)
// Fame: <N><pad>
// Account balance: <N>
// Gold balance: <N>
// Location: X=<%.3f> Y=<%.3f> Z=<%.3f>
//
// Sources: live controller/PlayerState/pawn calls + SCUM.db (user,
// user_profile, bank_account_registry_currencies). Emits nothing to the
// game chat, unlike the engine AdminCommand path.
std::string native_listplayers()
{
    RC::Unreal::UClass* pc_class = get_player_controller_class();
    if (!pc_class) return "listplayers: PlayerController class not resolvable";
    struct PlayerInfo {
        RC::Unreal::UObject* pc = nullptr;
        std::string steam_id;
    };
    std::vector<PlayerInfo> players;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;
        if (!klass->IsChildOf(pc_class)) return RC::LoopAction::Continue;
        if (obj->HasAnyFlags(RC::Unreal::EObjectFlags::RF_ClassDefaultObject))
            return RC::LoopAction::Continue;
        if (obj->GetFullName().find(L"Default__") != std::wstring::npos)
            return RC::LoopAction::Continue;
        std::string sid = controller_steam_id(obj);
        if (sid.empty()) return RC::LoopAction::Continue;
        players.push_back(PlayerInfo{ obj, sid });
        return RC::LoopAction::Continue;
    });
    if (players.empty()) {
        return "No players online.";
    }
    std::ostringstream os;
    for (size_t i = 0; i < players.size(); ++i) {
        RC::Unreal::UObject* pc = players[i].pc;
        std::string const& sid = players[i].steam_id;
        RC::Unreal::UObject* ps = resolve_object_prop(pc, L"PlayerState");
        RC::Unreal::UObject* pawn = resolve_object_prop(pc, L"Pawn");

        // Character name: PlayerState.GetPlayerName(), then
        // PlayerNamePrivate @768, then DB.
        std::string char_name;
        {
            std::wstring ws;
            if (ps && call_fstring_fn(ps, L"GetPlayerName", ws))
                char_name = log::narrow(ws);
        }
        if (char_name.empty() && ps && ps->GetClassPrivate()) {
            RC::Unreal::FProperty* nm_prop = find_property_in_chain(
                ps->GetClassPrivate(),
                RC::Unreal::FName(L"PlayerNamePrivate",
                                  RC::Unreal::EFindName::FName_Add));
            if (nm_prop && nm_prop->GetSize() == 16) {
                auto* fs = reinterpret_cast<RC::Unreal::FString*>(
                    reinterpret_cast<char*>(ps) + nm_prop->GetOffset_Internal());
                std::wstring ws = fs->wstring();
                if (!ws.empty()) char_name = log::narrow(ws);
            }
        }

        float fame_live_f = 0;
        bool has_fame_live = false;
        {
            // NOTE: GetFamePoints returns float (fame_points is REAL in
            // SCUM.db), not int32 — read 4 bytes as float.
            RC::Unreal::UFunction* fn = pc->GetFunctionByNameInChain(
                L"GetFamePoints");
            if (fn && fn->GetParmsSize() == 4) {
                void* buf = ::operator new(4);
                std::memset(buf, 0, 4);
                pc->ProcessEvent(fn, buf);
                std::uint16_t ret_off = fn->GetReturnValueOffset();
                if (ret_off != 0xFFFF) {
                    std::memcpy(&fame_live_f,
                                reinterpret_cast<char*>(buf) + ret_off,
                                sizeof(fame_live_f));
                    has_fame_live = true;
                }
                ::operator delete(buf);
            }
        }

        Vec3f loc;
        bool has_loc = pawn && call_fvector_fn(pawn, L"K2_GetActorLocation", loc);

        questdb::PlayerEconomy eco;
        questdb::player_economy(sid, eco);
        if (char_name.empty()) char_name = eco.char_name;
        std::string steam_name = eco.steam_name;
        if (steam_name.empty()) steam_name = char_name;
        long long fame = has_fame_live
            ? static_cast<long long>(fame_live_f)
            : (eco.has_fame ? eco.fame : 0);

        os << "\n " << (i + 1) << ". " << char_name
           << "\nSteam: " << steam_name << " (" << sid << ")\n";
        std::string fame_field = "Fame: " + std::to_string(fame);
        os << fame_field << std::string(
            fame_field.size() < 26 ? 26 - fame_field.size() : 1, ' ')
           << "\nAccount balance: " << eco.account
           << "\nGold balance: " << eco.gold << "\n";
        if (has_loc) {
            std::ostringstream ls;
            ls << std::fixed;
            ls.precision(3);
            ls << "Location: X=" << loc.x << " Y=" << loc.y << " Z=" << loc.z
               << "\n";
            os << ls.str();
        }
    }
    return os.str();
}

// Debug: dump property names (name@offset size) of a controller, its
// PlayerState and its Pawn into UE4SS.log. Used to discover the real
// property names (Name / Fame / balances / Location) for the ListPlayers
// format rebuild. Safe: reads metadata only.
void dump_struct_props(std::wstring const& owner, RC::Unreal::UStruct* s)
{
    if (!s) {
        log::line(L"dumpprops: " + owner + L" (null struct)");
        return;
    }
    RC::Unreal::FField* f = s->GetChildProperties();
    int n = 0;
    while (f && n < 300) {
        auto* prop = static_cast<RC::Unreal::FProperty*>(f);
        std::wstring line = L"dumpprops: " + owner + L"." +
            static_cast<std::wstring>(f->GetName()) + L" @" +
            std::to_wstring(prop->GetOffset_Internal()) + L" size=" +
            std::to_wstring(prop->GetSize());
        log::line(line);
        ++n;
        f = f->NextField();
    }
    log::line(L"dumpprops: " + owner + L" total=" + std::to_wstring(n));
}

RC::Unreal::UObject* dump_resolve_object_prop(RC::Unreal::UObject* obj,
                                              const wchar_t* prop_name)
{
    return resolve_object_prop(obj, prop_name);
}

std::string dump_controller_props(std::vector<std::string> const& args)
{
    log::line(L"dumpprops: build MARKER_R5 (scanner removed, clean)");
    RC::Unreal::UObject* pc = nullptr;
    if (args.size() >= 2 && args[1].size() == 17)
        pc = find_controller_by_steam_id(args[1]);
    if (!pc) pc = find_first_online_controller();
    if (!pc) return "dumpprops: no online controller";
    auto* cls = pc->GetClassPrivate();
    log::line(std::wstring(L"dumpprops: controller class=") +
              (cls ? cls->GetName() : L"?"));
    dump_struct_props(L"controller", cls);
    RC::Unreal::UObject* ps = dump_resolve_object_prop(pc, L"PlayerState");
    if (ps && ps->GetClassPrivate()) {
        log::line(std::wstring(L"dumpprops: playerstate class=") +
                  ps->GetClassPrivate()->GetName());
        dump_struct_props(L"playerstate", ps->GetClassPrivate());
    } else {
        log::line(L"dumpprops: no PlayerState object");
    }
    RC::Unreal::UObject* pawn = dump_resolve_object_prop(pc, L"Pawn");
    if (pawn && pawn->GetClassPrivate()) {
        log::line(std::wstring(L"dumpprops: pawn class=") +
                  pawn->GetClassPrivate()->GetName());
        dump_struct_props(L"pawn", pawn->GetClassPrivate());
    } else {
        log::line(L"dumpprops: no Pawn object");
    }
    // Candidate probing: FindProperty works even where ChildProperties
    // enumeration returns nothing. Probe likely names for the ListPlayers
    // fields (name / fame / balances) on each object.
    static const wchar_t* const kCandidates[] = {
        L"PlayerName", L"PlayerNamePrivate", L"SteamName", L"UserSteamName",
        L"OnlineName", L"CharacterName", L"PrisonerName", L"UserName",
        L"FamePoints", L"Fame", L"FamePoint",
        L"AccountBalance", L"GoldBalance", L"CurrencyBalance",
        L"BankBalance", L"WalletBalance", L"Money", L"Cash",
        L"UserId", L"SteamId", L"PlayerId",
        L"PlayerState", L"Pawn", L"Player",
    };
    auto probe_obj = [&](const wchar_t* tag, RC::Unreal::UObject* obj) {
        if (!obj || !obj->GetClassPrivate()) return;
        for (auto* cand : kCandidates) {
            RC::Unreal::FProperty* p = find_property_in_chain(
                obj->GetClassPrivate(),
                RC::Unreal::FName(cand, RC::Unreal::EFindName::FName_Add));
            if (p) {
                std::wstring line = L"dumpprops: HIT ";
                line += tag;
                line += L".";
                line += cand;
                line += L" @";
                line += std::to_wstring(p->GetOffset_Internal());
                line += L" size=";
                line += std::to_wstring(p->GetSize());
                log::line(line);
            }
        }
    };
    probe_obj(L"controller", pc);
    probe_obj(L"playerstate", ps);
    probe_obj(L"pawn", pawn);
    // Candidate UFunctions (location / name / stats getters).
    static const wchar_t* const kFuncCandidates[] = {
        L"GetActorLocation", L"GetPlayerName", L"GetPlayerStateName",
        L"GetFamePoints", L"GetAccountBalance", L"GetGoldBalance",
        L"GetUserId", L"GetLocation",
    };
    auto probe_fns = [&](const wchar_t* tag, RC::Unreal::UObject* obj) {
        if (!obj) return;
        for (auto* cand : kFuncCandidates) {
            RC::Unreal::UFunction* fn = obj->GetFunctionByNameInChain(cand);
            if (fn) {
                std::wstring label = L"dumpprops: FN ";
                label += tag;
                label += L".";
                label += cand;
                log_function_params(label, fn);
            }
        }
    };
    probe_fns(L"controller", pc);
    probe_fns(L"playerstate", ps);
    probe_fns(L"pawn", pawn);
    // Round 2: economy / nickname / location candidates, plus the Player
    // object (controller.Player @664) which may carry the Steam persona name.
    RC::Unreal::UObject* net_player = dump_resolve_object_prop(pc, L"Player");
    if (net_player && net_player->GetClassPrivate()) {
        log::line(std::wstring(L"dumpprops: player class=") +
                  net_player->GetClassPrivate()->GetName());
        dump_struct_props(L"player", net_player->GetClassPrivate());
    } else {
        log::line(L"dumpprops: no Player object");
    }
    static const wchar_t* const kCandidates2[] = {
        L"NickName", L"Nickname", L"Gold", L"Cash", L"CashBalance",
        L"Bank", L"Wallet", L"Account", L"Balance", L"MoneyBalance",
        L"RootComponent", L"RelativeLocation", L"Location", L"Position",
        L"Kills", L"Deaths", L"Score",
    };
    auto probe_obj2 = [&](const wchar_t* tag, RC::Unreal::UObject* obj) {
        if (!obj || !obj->GetClassPrivate()) return;
        for (auto* cand : kCandidates2) {
            RC::Unreal::FProperty* p = find_property_in_chain(
                obj->GetClassPrivate(),
                RC::Unreal::FName(cand, RC::Unreal::EFindName::FName_Add));
            if (p) {
                std::wstring line = L"dumpprops: HIT2 ";
                line += tag;
                line += L".";
                line += cand;
                line += L" @";
                line += std::to_wstring(p->GetOffset_Internal());
                line += L" size=";
                line += std::to_wstring(p->GetSize());
                log::line(line);
            }
        }
    };
    probe_obj2(L"controller", pc);
    probe_obj2(L"playerstate", ps);
    probe_obj2(L"pawn", pawn);
    probe_obj2(L"player", net_player);
    static const wchar_t* const kFuncCandidates2[] = {
        L"K2_GetActorLocation", L"GetPawn", L"GetPlayerState",
        L"GetFamePoints", L"GetCharacterName", L"GetSteamName",
    };
    auto probe_fns2 = [&](const wchar_t* tag, RC::Unreal::UObject* obj) {
        if (!obj) return;
        for (auto* cand : kFuncCandidates2) {
            RC::Unreal::UFunction* fn = obj->GetFunctionByNameInChain(cand);
            if (fn) {
                std::wstring label = L"dumpprops: FN2 ";
                label += tag;
                label += L".";
                label += cand;
                log_function_params(label, fn);
            }
        }
    };
    probe_fns2(L"controller", pc);
    probe_fns2(L"playerstate", ps);
    probe_fns2(L"pawn", pawn);
    probe_fns2(L"player", net_player);
    return "dumpprops: written to UE4SS.log";
}

} // anonymous namespace

// ===========================================================================
//  probe_admin_result_functions
// ===========================================================================
// Scan every live UFunction whose name matches an admin-output / result
// sink pattern and log it, so the engine_hooks to-be-hooked paths translate
// to whatever this SCUM build actually ships. Runs once during startup.
void probe_admin_result_functions()
{
    static bool done = false;
    if (done) return;
    done = true;

    RC::Unreal::UClass* ufunc_class = RC::Unreal::UFunction::StaticClass();
    int matched = 0;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        if (!obj->IsA(ufunc_class)) return RC::LoopAction::Continue;
        std::wstring nm  = obj->GetName();
        std::wstring cls = obj->GetClassPrivate()
            ? obj->GetClassPrivate()->GetName() : L"?";
        bool hit = false;
        static const wchar_t* const kNeedles[] = {
            L"Admin", L"Command", L"Result", L"Chat", L"Show", L"Broadcast",
        };
        for (auto* n : kNeedles) {
            if (nm.find(n) != std::wstring::npos) { hit = true; break; }
        }
        // Functions may live on RPC-channel or controller classes without
        // carrying one of the needles themselves - match the owner class too.
        if (!hit) {
            static const wchar_t* const kClassNeedles[] = {
                L"RpcChannel", L"RPCChannel", L"PlayerController",
                L"AConZGameState", L"ConZGameMode",
            };
            for (auto* n : kClassNeedles) {
                if (cls.find(n) != std::wstring::npos) { hit = true; break; }
            }
        }
        if (hit) {
            log::line(L"probe: admin-result fn " + obj->GetFullName() +
                      L"  [class=" + cls + L"]");
            matched++;
        }
        return RC::LoopAction::Continue;
    });
    log::line(L"probe: admin-result fn scan matched " + std::to_wstring(matched) +
              L" function(s)");
}

// ===========================================================================
//  build_verb_map
// ===========================================================================
std::uint64_t build_verb_map()
{
    std::lock_guard<std::mutex> lk(g_verb_mutex);
    if (g_verb_map_built) return g_verb_count;

    RC::Unreal::UClass* admin_cmd =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, L"/Script/SCUM.AdminCommand");
    if (!admin_cmd) {
        log::line(L"dispatch: AdminCommand base class not yet loaded - verb map will be built on the next attempt");
        return 0;
    }

    std::size_t scanned = 0, child_of = 0, name_match = 0, extracted = 0;
    std::vector<std::wstring> child_of_names;
    std::vector<std::wstring> admin_named;
    bool logged_base = false;
    bool logged_sub  = false;

    log_struct_fields(L"dispatch: AdminCommand base fields", admin_cmd);
    logged_base = true;

    // The reference transaction is defined by the cast flag
    // (EClassCastFlags::AdminCommand). Find the numeric value once so we can
    // filter with HasAllCastFlags instead of IsChildOf (which is false when
    // the base is still loading).
    std::uint64_t admin_cast = 0;
    {
        // SCUM registers AdminCommand with its own ClassCastFlag bit. The
        // stub's enum window only reaches 1<<7, so we scan every bit up to
        // 1<<63 and keep the first one the base class declares.
        for (std::uint64_t probe = 0x0400ull; probe; probe <<= 1) {
            if (admin_cmd->HasAllCastFlags(
                    static_cast<RC::Unreal::EClassCastFlags>(probe))) {
                admin_cast = probe;
                break;
            }
        }
    }
    log::line(L"dispatch: AdminCommand cast flag = " +
              (admin_cast ? std::to_wstring(admin_cast) : L"?? (fallback to IsChildOf)"));

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        ++scanned;
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;

        bool is_child = klass->IsChildOf(admin_cmd);
        bool has_flag = admin_cast != 0 &&
            klass->HasAllCastFlags(static_cast<RC::Unreal::EClassCastFlags>(admin_cast));
        std::wstring wname = klass->GetName();
        std::string  name  = log::narrow(wname);
        std::string  lower = to_lower(name);
        bool named = lower.find("admincommand") != std::string::npos;

        if (is_child) ++child_of;
        if (named) ++name_match;
        if (child_of_names.size() < 8 && is_child) child_of_names.push_back(wname);
        if (admin_named.size() < 8 && named) admin_named.push_back(wname);
        if (has_flag && !logged_sub) {
            log_struct_fields(L"dispatch: first AdminCommand subclass fields", klass);
            log::line(L"dispatch: first AdminCommand subclass name='" + wname +
                      L"' child=" + (is_child ? L"Y" : L"N") +
                      L" castflag=" + (has_flag ? L"Y" : L"N"));
            logged_sub = true;
        }

        // Match by super-class OR cast flag OR the naming convention.
        if (!is_child && !has_flag && !named) return RC::LoopAction::Continue;

        // Extract the verb from the class name: "AdminCommand_ListSquads" ->
        // "listsquads". BP subclasses carry a trailing "_c".
        constexpr const char* kPrefix = "admincommand_";
        if (lower.rfind(kPrefix, 0) != 0) return RC::LoopAction::Continue;
        std::string verb = lower.substr(std::strlen(kPrefix));
        if (verb.size() >= 2 && verb.compare(verb.size() - 2, 2, "_c") == 0)
            verb.resize(verb.size() - 2);

        // Resolve the execute entry point on the *class* (not the CDO):
        // FindFunction("Execute")/GetFuncPtr semantics, then
        // executed the constructed command instance on the game thread.
        // SCUM AdminCommand subclasses do not carry an "Execute" function in
        // the UClass tree, so verbs match by name alone and execution falls
        // back to the chat pipeline (see dispatch_command).
        RC::Unreal::UFunction* fn = probe_execute_fn(klass);

        g_verb_map[verb] = VerbEntry{ klass, fn };
        ++extracted;
        return RC::LoopAction::Continue;
    });

    log::line(L"dispatch: verb map scan: scanned " + std::to_wstring(scanned) +
              L" objects, " + std::to_wstring(child_of) +
              L" AdminCommand subclasses, " + std::to_wstring(name_match) +
              L" with 'admincommand' in the name");
    if (!child_of_names.empty()) {
        std::wstring s = L"dispatch:   AdminCommand subclasses: ";
        for (auto const& n : child_of_names) { s += n; s += L"  "; }
        log::line(s);
    }
    if (!admin_named.empty()) {
        std::wstring s = L"dispatch:   classes with 'admincommand' in name: ";
        for (auto const& n : admin_named) { s += n; s += L"  "; }
        log::line(s);
    }

    if (extracted == 0) {
        log::line(L"dispatch: extracted 0 verbs - will retry on the next dispatch");
        return 0;
    }

    g_verb_map_built = true;
    g_verb_count     = extracted;
    log::line(L"dispatch: verb map built - " + std::to_wstring(extracted) +
              L" command(s) discovered (scanned " + std::to_wstring(scanned) +
              L" objects, " + std::to_wstring(child_of) +
              L" AdminCommand subclasses)");

    // Friendly aliases (USAGE.md names → raw class-derived verbs).
    // The original SCUM-RCON mapped these so users could type the short form.
    struct Alias { const char* alias; const char* target; };
    static constexpr Alias kAliases[] = {
        {"SetImmortality",            "setprisonerimmortality"},
        {"SetInfiniteStamina",        "setprisonerinfinitestamina"},
        {"SetInfiniteOxygen",         "setprisonerinfiniteoxygen"},
        {"SetAttributes",             "setprisonerattributes"},
        {"DisableBodyEffects",        "disableprisonerbodyeffects"},
        {"EnableBodyEffects",         "enableprisonerbodyeffects"},
        {"AddBleedingInjury",         "addprisonerbodyeffect"},
        {"RemoveBleedingInjury",      "removeprisonerbodyeffect"},
        {"AddRadiationPresence",      "addprisonerbodyeffect"},
        {"Knockout",                  "knockoutprisoner"},
        {"Suicide",                   "crashmajestically"},
        {"ResetAchievements",         "resetachievements"},
        {"ListVehicles",              "listspawnedvehicles"},
        {"ListAnimals",               "listspawnedanimals"},
        {"ListNPCs",                  "listspawnedarmednpcs"},
        {"Whois",                     "getuserid"},
        {"ShowNameplates",            "shownameplates"},
        {"SetDecayTimeDilation",      "setdecaytimedilation"},
        {"SetFarmingSimSpeed",        "setfarmingsimulationspeed"},
        {"SetMetabolismSimSpeed",     "setprisonermetabolismsimulationspeed"},
    };
    for (auto const& a : kAliases) {
        std::string alias_lower = to_lower(a.alias);
        auto it = g_verb_map.find(a.target);
        if (it != g_verb_map.end() && g_verb_map.find(alias_lower) == g_verb_map.end()) {
            g_verb_map[alias_lower] = it->second;
        }
    }
    return extracted;
}

// ===========================================================================
//  Native executor resolution (Stage 24)
// ===========================================================================
// The SCUM command-executor function is resolved dynamically via a pattern
// scanner searching the game image's .text for a 24-byte sequence.
// The sequence was verified unique in SCUMServer.exe (single match at
// .text RVA 0x187F384). The targeted function takes (command UObject,
// context array) and executes the command via a vtable tail-call.
//
//   pattern: 10 48 89 74 24 18 57 48 83 EC 30 48 8B F9 0F 29
//            74 24 20 48 8D 4C 24 40
//   noted  : the first byte (0x10) is the tail of "mov [rsp+0x10], rbx";
//            the actual function prologue starts 4 bytes earlier.
std::uintptr_t resolve_native_executor()
{
    static std::uintptr_t cached = 0;
    static bool tried = false;
    if (tried) return cached;
    tried = true;

    static const std::uint8_t kSig[] = {
        0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
        0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x0F, 0x29,
        0x74, 0x24, 0x20, 0x48, 0x8D, 0x4C, 0x24, 0x40
    };
    static constexpr std::size_t kSigLen = sizeof(kSig);
    // 4 bytes before the matched signature is the real function start.
    static constexpr std::size_t kPrologueBack = 4;

    HMODULE img = ::GetModuleHandleW(nullptr);   // primary image (SCUMServer.exe)
    if (!img) {
        log::line(L"executor: GetModuleHandleW(nullptr) failed");
        return 0;
    }

    auto const* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log::line(L"executor: bad DOS header");
        return 0;
    }
    auto const* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const std::uint8_t*>(img) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        log::line(L"executor: bad NT header");
        return 0;
    }

    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(img);
    const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sh) {
        char name[9] = {0};
        std::memcpy(name, sh->Name, 8);
        if (std::string_view(name) != ".text") continue;

        const std::uint8_t* text = base + sh->VirtualAddress;
        std::size_t size = sh->Misc.VirtualSize;

        for (std::size_t off = kPrologueBack;
             off + kSigLen <= size; ++off) {
            if (std::memcmp(text + off, kSig, kSigLen) == 0) {
                std::uintptr_t func = reinterpret_cast<std::uintptr_t>(base) +
                    sh->VirtualAddress + off - kPrologueBack;
                std::wostringstream os;
                os << L"executor: resolved native command executor @ 0x" << std::hex
                   << func << L" (sig RVA 0x" << (sh->VirtualAddress + off)
                   << L", func RVA 0x" << (sh->VirtualAddress + off - kPrologueBack)
                   << L")";
                log::line(os.str());
                cached = func;
                return func;
            }
        }
        log::line(L"executor: signature not found in primary image .text");
        return 0;
    }

    log::line(L"executor: no .text section in primary image");
    return 0;
}

// ===========================================================================
//  dispatch_direct_executor  (Stage 24 / Path 3, headless)
// ===========================================================================
// Build a concrete command instance with StaticConstructObject, fill a
// TArray<FString> command-context from the raw arguments, and call the
// pre-resolved native executor as
//    executor(command_instance, &context_array, nullptr, &exception_flag)
// The executor executes the command via a vtable tail-call ([vtable+0x288]).
// No online player is required, so this is the headless fallback path.
bool dispatch_direct_executor(RC::Unreal::UClass* klass,
                              std::vector<std::string> const& args,
                              std::vector<std::string>& out_lines)
{
    std::uintptr_t exec = resolve_native_executor();
    if (!exec) {
        log::line(L"dispatch: command executor not resolved");
        return false;
    }
    if (!klass) {
        log::line(L"dispatch: direct executor: null command class");
        return false;
    }
    // NOTE: dispatch_command always runs on the game thread via the
    // EngineTick drain (game_thread::drain_one), so we deliberately do NOT
    // gate on RC::Unreal::IsInGameThreadRaw() here - on this UE4SS build that
    // check reports false even on the real game thread, which would dead-end
    // the headless executor path.

    RC::Unreal::FStaticConstructObjectParameters params;
    params.Class                           = klass;
    params.Outer                           = nullptr;   // => transient package
    params.Name                            = RC::Unreal::FName();  // NAME_None
    params.SetFlags                        = SCO_RF_Transient;
    params.InternalSetFlags                = 0;
    params.bCopyTransientsFromClassDefaults = false;
    params.bAssumeTemplateIsArchetype      = false;
    params.Template                        = nullptr;   // => class default object
    params.InstanceGraph                   = nullptr;
    params.ExternalPackage                 = nullptr;

    RC::Unreal::UObject* cmd =
        RC::Unreal::UObjectGlobals::StaticConstructObject(params);
    if (!cmd) {
        log::line(L"dispatch: command instance construction failed");
        return false;
    }
    log::line(std::wstring(L"dispatch: direct executor: constructed ") +
              cmd->GetFullName());

    // Command context = the raw arguments (verb excluded - the verb is already
    // encoded in the command class). TArray<FString> = 16-byte header
    // {FString* Data; int32 Num; int32 Max}; FString is 16 bytes in UE4.27.
    std::vector<RC::Unreal::FString> ctx_strings;
    ctx_strings.reserve(args.size() > 0 ? args.size() - 1 : 0);
    for (std::size_t i = 1; i < args.size(); ++i) {
        ctx_strings.emplace_back(log::widen(args[i]));
    }
    struct FStringArray {
        RC::Unreal::FString* Data;
        std::int32_t         Num;
        std::int32_t         Max;
    };
    FStringArray ctx;
    ctx.Data = ctx_strings.empty() ? nullptr : ctx_strings.data();
    ctx.Num  = static_cast<std::int32_t>(ctx_strings.size());
    ctx.Max  = ctx.Num;

    // Ensure a synthetic caller (ConZPlayerController + pawn) exists inside the
    // world. Commands such as listplayers enumerate the world's controllers
    // and block forever when no controller/pawn is present headless.
    // Gated behind config ([synth] pc=true): spawning a player controller +
    // pawn on a dedicated server crashed it, so this is opt-in.
    if (scum_rcon::config::current().synth.pc) {
        RC::Unreal::UObject* caller = synth::get_or_build_companion_controller();
        if (caller) {
            log::line(std::wstring(L"dispatch: direct executor: using synth caller ") +
                      caller->GetFullName());
        } else {
            log::line(L"dispatch: direct executor: synth caller unavailable");
        }
    }

    using ResolvedExecutorFn = std::uintptr_t (*)(
        void*, void*, void*, std::uint8_t*);
    auto fn = reinterpret_cast<ResolvedExecutorFn>(exec);
    std::uint8_t exc = 0;
    std::uintptr_t rc = fn(cmd, &ctx, nullptr, &exc);

    out_lines = capture_snapshot();
    {
        std::wostringstream os;
        os << L"dispatch: direct executor: exec rc=0x" << std::hex << rc
           << L" excflag=" << static_cast<unsigned>(exc)
           << L" captured=" << std::dec << out_lines.size() << L" line(s)";
        log::line(os.str());
    }
    if (!out_lines.empty()) {
        log::line(std::wstring(L"dispatch: '") + log::widen(args[0]) +
                  L"' executed via direct executor (" +
                  std::to_wstring(out_lines.size()) + L" line(s) captured)");
    }
    // exception flag set => the target threw; fall back to the chat pipeline.
    return exc == 0;
}

// ===========================================================================
//  find_first_online_controller
// ===========================================================================
RC::Unreal::UObject* find_first_online_controller()
{
    RC::Unreal::UClass* pc_class = get_player_controller_class();
    if (!pc_class) return nullptr;

    RC::Unreal::UObject* first = nullptr;
    int total = 0;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;
        if (!klass->IsChildOf(pc_class)) return RC::LoopAction::Continue;
        ++total;
        if (total <= 30) {
            log::line(std::wstring(L"dispatch: controller candidate ") +
                      obj->GetFullName() +
                      (obj->HasAnyFlags(RC::Unreal::EObjectFlags::RF_ClassDefaultObject)
                           ? L" (CDO)" : L""));
        }
        // Exclude class-default objects and any object nested inside one.
        bool is_cdo_nested = obj->HasAnyFlags(
            RC::Unreal::EObjectFlags::RF_ClassDefaultObject) ||
            obj->GetFullName().find(L"Default__") != std::wstring::npos;
        if (is_cdo_nested) return RC::LoopAction::Continue;
        if (!first) first = obj;
        return RC::LoopAction::Continue;
    });
    log::line(std::wstring(L"dispatch: controller scan saw ") +
              std::to_wstring(total) + L" object(s); caller = " +
              (first ? first->GetName() : L"(none; all Default__)"));
    return first;
}

// ===========================================================================
//  find_controller_by_steam_id
// ===========================================================================
RC::Unreal::UObject* find_controller_by_steam_id(std::string const& steam_id)
{
    RC::Unreal::UClass* pc_class = get_player_controller_class();
    if (!pc_class) return nullptr;

    RC::Unreal::UObject* found = nullptr;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;
        if (!klass->IsChildOf(pc_class)) return RC::LoopAction::Continue;
        if (controller_steam_id(obj) == steam_id) {
            found = obj;
            return RC::LoopAction::Break;
        }
        return RC::LoopAction::Continue;
    });
    return found;
}

// ---- Chat pipeline ------------------------------------------------------
// SCUM dispatches every admin command through the *chat* RPC:
//   PlayerRpcChannel:Chat_Server_ProcessAdminCommand
// which the engine resolves to the matching AdminCommand subclass.
// The chat pipeline is used in preference to a direct Execute call:
//   "dispatch: '{}' executed via chat pipeline (caller {}, {} line(s) captured)"
//   "player RPC channel is null"
//   "chat-pipeline (ProcessEvent): dispatch target unavailable"
// Both a channel object and the UFunction are resolved live.

RC::Unreal::UFunction* g_chat_fn = nullptr;

RC::Unreal::UFunction* get_chat_process_admin_cmd_fn()
{
    if (g_chat_fn) return g_chat_fn;
    log_function_params(
        L"dispatch: probe Chat_Server_ProcessAdminCommand",
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/SCUM.PlayerRpcChannel:Chat_Server_ProcessAdminCommand"));
    log_function_params(
        L"dispatch: probe Chat_Server_BroadcastChatMessage",
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/SCUM.PlayerRpcChannel:Chat_Server_BroadcastChatMessage"));
    g_chat_fn = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
        nullptr, L"/Script/SCUM.PlayerRpcChannel:Chat_Server_ProcessAdminCommand");
    return g_chat_fn;
}

// Find a PlayerRpcChannel instance belonging to an online player. CDO and
// template objects are excluded: calling a server RPC on the class-default
// object crashes the engine ("player RPC channel is null").
RC::Unreal::UObject* find_rpc_channel_for(RC::Unreal::UObject* pc)
{
    RC::Unreal::UClass* ch_class =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, L"/Script/SCUM.PlayerRpcChannel");
    if (!ch_class) ch_class =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, L"/Script/ConZ.PlayerRpcChannel");
    if (!ch_class) {
        log::line(L"dispatch: PlayerRpcChannel class not resolvable");
        return nullptr;
    }

    RC::Unreal::UObject* first = nullptr;
    int total = 0, live = 0;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;
        if (!klass->IsChildOf(ch_class)) return RC::LoopAction::Continue;
        ++total;
        std::wstring full = obj->GetFullName();
        log::line(std::wstring(L"dispatch: channel candidate ") + full +
                  (obj->HasAnyFlags(RC::Unreal::EObjectFlags::RF_ClassDefaultObject)
                       ? L" (CDO)" : L""));
        // Exclude class-default channel instances *and* any channel owned by a
        // Default__ object (e.g. "...Default__ConZPlayerController:PlayerRpcChannel").
        if (full.find(L"Default__") != std::wstring::npos) return RC::LoopAction::Continue;
        ++live;
        if (!first) first = obj;
        return RC::LoopAction::Continue;
    });

    log::line(std::wstring(L"dispatch: PlayerRpcChannel scan found ") +
              std::to_wstring(total) + L" object(s), " +
              std::to_wstring(live) + L" live (excl. CDO)");
    if (!first) {
        log::line(L"dispatch: player RPC channel is null");
        return nullptr;
    }
    (void)pc;
    return first;
}

// Execute argv via the chat pipeline. cmdline is the full command line
// including the verb, exactly as an admin would type it in chat.
bool dispatch_chat_pipeline(RC::Unreal::UObject* pc,
                            std::vector<std::string> const& args,
                            std::string const& cmdline,
                            std::vector<std::string>& out_lines)
{
    RC::Unreal::UFunction* fn = get_chat_process_admin_cmd_fn();
    if (!fn) return false;

    RC::Unreal::UObject* channel = find_rpc_channel_for(pc);
    if (!channel) {
        log::line(L"dispatch: player RPC channel is null");
        return false;
    }

    // SendChatLineToPlayer-style caller; the chat pipeline needs the
    // PlayerRpcChannel object as `self`. The single 16-byte (FString)
    // parameter carries the full command text; its name varies by SCUM
    // build, so we place by position rather than by name.
    void* buf = make_params(fn);
    std::wstring wcmd = log::widen(cmdline);
    int off = set_first_string_param(fn, buf, wcmd);
    if (off < 0) {
        cleanup_params(buf);
        log::line(L"dispatch: chat pipeline params unresolved - cannot invoke");
        return false;
    }

    channel->ProcessEvent(fn, buf);
    cleanup_params(buf);
    out_lines = capture_snapshot();
    log::line(std::wstring(L"dispatch: executed via chat pipeline (caller ") +
              (pc ? pc->GetName() : L"null") + L", " +
              std::to_wstring(out_lines.size()) + L" line(s) captured)");
    return true;
}

// Redirect the current command to an online player's controller if the last
// argument is a 17-digit SteamID (chat-pipeline requirement: player must be
// online). Returns the controller or nullptr.
RC::Unreal::UObject* caller_for_steam_id(std::vector<std::string> const& args)
{
    if (args.size() < 2) return nullptr;
    std::string const& last = args.back();
    if (last.size() != 17) return nullptr;
    if (last.find_first_not_of("0123456789") != std::string::npos)
        return nullptr;
    return find_controller_by_steam_id(last);
}

// ===========================================================================
//  dispatch_sendchat
//  Routes through the chat pipeline (Chat_Server_ProcessAdminCommand) to
//  avoid the MiscStatics:SendChatLineToPlayer developer check.
// ===========================================================================
Result dispatch_sendchat(std::vector<std::string> const& args)
{
    Result r;
    if (args.size() < 3) {
        r.reply = "usage: SendChat <type 0-7> \"<message>\" [<17-digit SteamID>] "
                  "(no SteamID = all players; 0 white, 2 blue, 3 green, "
                  "4 yellow, 6 orange, 7 red)";
        return r;
    }
    int type = 0;
    try {
        type = std::stoi(args[1]);
    } catch (...) {
        r.reply = "chat type must be a number 0-7";
        return r;
    }
    if (type < 0 || type > 7) {
        r.reply = "chat type must be a number 0-7";
        return r;
    }
    std::string const& message = args[2];
    if (message.empty()) {
        r.reply = "SendChat needs a message";
        return r;
    }

    // Detect trailing 17-digit SteamID among remaining args (args[3..N-1]).
    std::string steam_id;
    std::string full_msg = message;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i].size() == 17 &&
            args[i].find_first_not_of("0123456789") == std::string::npos) {
            steam_id = args[i];
        } else {
            if (!full_msg.empty()) full_msg += " ";
            full_msg += args[i];
        }
    }

    // SendChat: multiple strategies, tried in order:
    // 1. MiscStatics:SendChatLineToPlayer (static, requires developer status)
    // 2. ClientShowAdminCommandResult / ClientShowCommandResult (Client RPC)
    // 3. MiscStatics:BroadcastChatLine (static broadcast)
    std::wstring wmsg = log::widen(full_msg);

    auto* statics_class =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, L"/Script/SCUM.MiscStatics");
    RC::Unreal::UObject* statics_cdo = statics_class
        ? statics_class->GetClassDefaultObject().ObjectPtr : nullptr;

    // Find player controller(s).
    std::vector<RC::Unreal::UObject*> controllers;
    if (!steam_id.empty()) {
        RC::Unreal::UObject* pc = find_controller_by_steam_id(steam_id);
        if (!pc) {
            r.reply = "no online player with SteamID " + steam_id;
            return r;
        }
        controllers.push_back(pc);
    } else {
        // Use the same controller discovery as listplayers.
        auto* pc_class = get_player_controller_class();
        if (pc_class) {
            RC::Unreal::UObjectGlobals::ForEachUObject(
                [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction {
                auto* klass = obj->GetClassPrivate();
                if (!klass) return RC::LoopAction::Continue;
                if (!klass->IsChildOf(pc_class)) return RC::LoopAction::Continue;
                if (obj->HasAnyFlags(RC::Unreal::EObjectFlags::RF_ClassDefaultObject))
                    return RC::LoopAction::Continue;
                controllers.push_back(obj);
                return RC::LoopAction::Continue;
            });
        }
    }

    if (controllers.empty()) {
        r.reply = "SendChat: no online player controllers";
        return r;
    }

    // Strategy 1: SendChatLineToPlayer (PlayerController@0, Text@8, ChatType@24,
    //   ShouldCopyToClientClipboard@25)
    RC::Unreal::UFunction* send_fn =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr, L"/Script/SCUM.MiscStatics:SendChatLineToPlayer");
    if (send_fn && statics_cdo) {
        log_function_params(L"dispatch: SendChat SendChatLineToPlayer", send_fn);
        int sent = 0;
        for (auto* pc : controllers) {
            void* buf = make_params(send_fn);
            set_object_param(send_fn, buf, L"PlayerController", pc);
            set_string_param(send_fn, buf, L"Text", wmsg);
            auto* ct = send_fn->FindProperty(
                RC::Unreal::FName(L"ChatType", RC::Unreal::EFindName::FName_Add));
            if (ct) {
                std::uint8_t ch = static_cast<std::uint8_t>(type);
                std::memcpy(reinterpret_cast<char*>(buf) + ct->GetOffset_Internal(),
                            &ch, sizeof(ch));
            }
            auto* scc = send_fn->FindProperty(
                RC::Unreal::FName(L"ShouldCopyToClientClipboard",
                    RC::Unreal::EFindName::FName_Add));
            if (scc) {
                std::uint8_t v = 0;
                std::memcpy(reinterpret_cast<char*>(buf) + scc->GetOffset_Internal(),
                            &v, sizeof(v));
            }
            statics_cdo->ProcessEvent(send_fn, buf);
            cleanup_params(buf);
            ++sent;
        }
        r.ok = true;
        r.reply = "SendChat: delivered to " + std::to_string(sent) + " player(s) [SendChatLineToPlayer]";
        return r;
    }

    // Strategy 2: ClientShowAdminCommandResult on PlayerController
    RC::Unreal::UFunction* client_fn =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/SCUM.ConZPlayerController:ClientShowAdminCommandResult");
    if (!client_fn) {
        client_fn = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/ConZ.ConZPlayerController:ClientShowAdminCommandResult");
    }
    if (client_fn) {
        log_function_params(L"dispatch: SendChat ClientShowAdminCommandResult", client_fn);
        int sent = 0;
        for (auto* pc : controllers) {
            void* buf = make_params(client_fn);
            set_string_param(client_fn, buf, L"Message", wmsg);
            pc->ProcessEvent(client_fn, buf);
            cleanup_params(buf);
            ++sent;
        }
        r.ok = true;
        r.reply = "SendChat: sent to " + std::to_string(sent) + " player(s) [ClientShowAdminCommandResult]";
        return r;
    }

    // Strategy 3: ClientShowCommandResult
    RC::Unreal::UFunction* client_fn2 =
        RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/SCUM.ConZPlayerController:ClientShowCommandResult");
    if (!client_fn2) {
        client_fn2 = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
            nullptr,
            L"/Script/ConZ.ConZPlayerController:ClientShowCommandResult");
    }
    if (client_fn2) {
        log_function_params(L"dispatch: SendChat ClientShowCommandResult", client_fn2);
        int sent = 0;
        for (auto* pc : controllers) {
            void* buf = make_params(client_fn2);
            set_string_param(client_fn2, buf, L"Message", wmsg);
            pc->ProcessEvent(client_fn2, buf);
            cleanup_params(buf);
            ++sent;
        }
        r.ok = true;
        r.reply = "SendChat: sent to " + std::to_string(sent) + " player(s) [ClientShowCommandResult]";
        return r;
    }

    r.reply = "SendChat: no usable function found (SendChatLineToPlayer, ClientShow*, BroadcastChatLine all unavailable)";
    return r;
}

// ===========================================================================
//  dispatch_command -- top-level entry
// ===========================================================================
namespace {
std::string dump_controller_props(std::vector<std::string> const& args);
}
std::string dispatch_command(std::string const& cmdline)
{
    try {
    if (!g_verb_map_built) build_verb_map();

    auto args = split_args(cmdline);
    if (args.empty()) return "ERR: empty command";

    std::string verb = to_lower(args[0]);

    // ---- Custom verbs ----
    if (verb == "sendchat") {
        return dispatch_sendchat(args).reply;
    }
    if (verb == "listcommands") {
        std::string out = "Available commands (" + std::to_string(g_verb_map.size()) + "):\n";
        for (auto const& [k, v] : g_verb_map) {
            out += k + "\n";
        }
        return out;
    }
    if (verb == "unstuck") {
        if (args.size() < 2)
            return "usage: Unstuck <17-digit SteamID>  (player must be online)";
        return "Unstuck: headless pawn teleport is not supported in this build; "
               "have the player use the in-game /unstuck command.";
    }
    if (verb == "dumpprops") {
        return dump_controller_props(args);
    }
    if (verb == "listsquads") {
        return questdb::list_squads();
    }
    if (verb == "findquestlockouts") {
        return questdb::format_lockouts(
            questdb::find_quest_lockouts(config::current().quests.blocked));
    }
    if (verb == "runquestunstick") {
        auto r = questdb::run_quest_unstick(
            config::current().quests.blocked,
            config::current().quests.auto_unstick_max_delete,
            config::current().quests.auto_unstick_dry_run);
        return "unstick: " + std::to_string(r.matches) + " match(es), " +
               std::to_string(r.deleted) + " deleted" +
               (r.aborted ? " (ABORTED - circuit breaker)" : "");
    }
    if (verb == "deleteactivequestsforuser") {
        if (args.size() < 2)
            return "error: usage: DeleteActiveQuestsForUser <17-digit SteamID>";
        long long n = questdb::delete_active_quests_for_user(args[1]);
        return "questdb: deleted " + std::to_string(n) +
               " active_quest row(s) for SteamID " + args[1];
    }
    // NOTE: no early "listplayers" intercept here on purpose. ListPlayers
    // must go through the engine AdminCommand + output capture so the reply
    // keeps the original format (name / Steam (id) / Fame / balances /
    // Location) that managers and bots parse. The native enumerator below
    // (native_listplayers) is fallback only.

    // ---- AdminCommand subclasses ----
    // ListPlayers is native-only: the engine reply would be emitted as
    // game-visible chat lines (direct native call, invisible to UE4SS
    // hooks) AND the RCON reply must not appear in any player's chat.
    // The native enumerator below rebuilds the original reply format.
    if (verb == "listplayers") return native_listplayers();
    // Same for ListSpawnedVehicles (polled ~1/sec by managers): native
    // DB-driven reply in the exact engine text format, no game-chat spam.
    if (verb == "listspawnedvehicles" || verb == "listvehicles")
        return questdb::list_spawned_vehicles();
    auto it = g_verb_map.find(verb);
    if (it == g_verb_map.end()) {
        return "ERR: unknown command '" + verb + "'";
    }

    capture_clear();
    std::vector<std::string> lines;

    // Path 1: the subclass exposes a real Execute-style UFunction -> direct
    // ProcessEvent on the CDO with the raw command line as CommandArgs.
    if (it->second.fn) {
        RC::Unreal::UObject* cdo =
            it->second.klass->GetClassDefaultObject().ObjectPtr;
        if (!cdo) {
            return "ERR: no class-default object for '" + verb + "'";
        }
        call_ufunction_string(cdo, it->second.fn, L"CommandArgs",
                              log::widen(cmdline));
        lines = capture_snapshot();
        if (!lines.empty()) {
            std::ostringstream os;
            for (auto const& l : lines) os << l << "\n";
            return os.str();
        }
    }

    // Path 2: chat pipeline. SCUM resolves the AdminCommand subclass from the
    // raw command text via the chat processing system, which applies the
    // command to the target player. This is the proven path that actually
    // executes commands with game-world side effects (godmode, teleport, spawn,
    // etc.). Requires an online player controller with an RPC channel.
    // Explicit 17-digit SteamID wins, else the first online controller.
    RC::Unreal::UObject* caller = caller_for_steam_id(args);
    if (!caller) caller = find_first_online_controller();
    if (dispatch_chat_pipeline(caller, args, cmdline, lines)) {
        if (!lines.empty()) {
            std::ostringstream os;
            for (auto const& l : lines) os << l << "\n";
            return os.str();
        }
        return verb + ": (executed, no output)";
    }

    // Path 3: direct native executor (headless fallback). Constructs a real
    // command instance and calls the pre-resolved SCUM executor directly.
    // Commands execute but may have no visible effect without a game-world
    // caller context. Used as fallback when no player is online.
    if (dispatch_direct_executor(it->second.klass, args, lines)) {
        if (!lines.empty()) {
            std::ostringstream os;
            for (auto const& l : lines) os << l << "\n";
            return os.str();
        }
        return verb + ": (executed, no output)";
    }

    return "ERR: '" + verb + "' could not be executed (no Execute fn, "
           "direct executor / chat pipeline unavailable)";
    }
    catch (std::exception const& e) {
        return std::string("ERR: exception during dispatch: ") + e.what();
    }
    catch (...) {
        return "ERR: unknown exception during dispatch";
    }
}

// ===========================================================================
//  Capture buffer (written by engine_hooks)
// ===========================================================================
void capture_append(std::string line)
{
    std::lock_guard<std::mutex> lk(g_capture_mutex);
    g_capture_lines.push_back(std::move(line));
}

void capture_clear()
{
    std::lock_guard<std::mutex> lk(g_capture_mutex);
    g_capture_lines.clear();
}

std::vector<std::string> capture_snapshot()
{
    std::lock_guard<std::mutex> lk(g_capture_mutex);
    return g_capture_lines;
}

} // namespace scum_rcon::dispatch