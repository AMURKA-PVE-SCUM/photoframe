// synth.cpp -- synthetic controller + character fabrication
//
// SCUM's AdminCommand system needs a caller (a ConZPlayerController inside the
// world) for some commands. When no real player is online we fabricate one:
// spawn a ConZPlayerController and a ConZCharacter through
// GameplayStatics::BeginSpawningActorFromClass / FinishSpawningActor (so they
// are properly registered actors in the world), then Controller::Possess the
// character with the controller.
//
// NOTE on the class path: the Blueprint
//   /Game/ConZ_Files/Blueprints/PlayerControllers/BP_ConZPlayerController
// which is not loaded on a dedicated headless server in this build. We resolve
// the always-loaded native /Script/SCUM.ConZPlayerController (falling back to
// /Script/Engine.PlayerController) instead, mirroring the port's own
// get_player_controller_class().
//
// Cache is validated on every use; if either object is gone / the world
// changed, the pair is rebuilt.
#include "synth.h"
#include "dispatch.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace scum_rcon::synth {

namespace {

struct Cache {
    std::mutex           m;
    RC::Unreal::UObject* controller = nullptr;
    RC::Unreal::UObject* character  = nullptr;
    RC::Unreal::UObject* world      = nullptr;
};
Cache g_cache;

// Native (always-loaded) classes. BP path is NOT loaded on dedicated servers.
constexpr wchar_t const* kControllerClassPath_Scum = L"/Script/SCUM.ConZPlayerController";
constexpr wchar_t const* kControllerClassPath_Engine = L"/Script/Engine.PlayerController";
constexpr wchar_t const* kCharacterClassPath_Scum   = L"/Script/SCUM.ConZCharacter";
constexpr wchar_t const* kCharacterClassPath_ConZ   = L"/Script/ConZ.ConZCharacter";
constexpr wchar_t const* kGameplayStaticsPath       = L"/Script/Engine.GameplayStatics";
constexpr wchar_t const* kUserProfileClassPath       = L"/Script/SCUM.UserProfile";
// Build an identity FTransform in a 0x60-byte buffer (UE4 layout).
void fill_identity_transform(char* buf)
{
    std::memset(buf, 0, 0x60);
    // FQuat Rotation (X,Y,Z,W) -- identity
    float* r = reinterpret_cast<float*>(buf + 0x00);
    r[0] = 0.f; r[1] = 0.f; r[2] = 0.f; r[3] = 1.f;
    // FVector Translation (buf + 0x14)
    float* t = reinterpret_cast<float*>(buf + 0x14);
    t[0] = 0.f; t[1] = 0.f; t[2] = 0.f;
    // FVector Scale3D (buf + 0x24)
    float* s = reinterpret_cast<float*>(buf + 0x24);
    s[0] = 1.f; s[1] = 1.f; s[2] = 1.f;
}

RC::Unreal::UObject* find_classes(RC::Unreal::UClass** ctrl,
                                  RC::Unreal::UClass** chr)
{
    *ctrl = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
        nullptr, kControllerClassPath_Scum);
    if (!*ctrl) {
        *ctrl = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, kControllerClassPath_Engine);
    }
    *chr = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
        nullptr, kCharacterClassPath_Scum);
    if (!*chr) {
        *chr = RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UClass>(
            nullptr, kCharacterClassPath_ConZ);
    }
    return (*ctrl && *chr) ? *ctrl : nullptr;
}

// Find a static function / a UClass CDO we can ProcessEvent on.
RC::Unreal::UFunction* find_ufunction(const wchar_t* path)
{
    return RC::Unreal::UObjectGlobals::FindObject<RC::Unreal::UFunction>(
        nullptr, path);
}

// GameplayStatics deferred spawn. Returns the spawned actor or nullptr.
RC::Unreal::UObject* spawn_actor(RC::Unreal::UClass* class_to_spawn,
                                 RC::Unreal::UObject* world)
{
    RC::Unreal::UClass* gs_class = RC::Unreal::UObjectGlobals::FindObject<
        RC::Unreal::UClass>(nullptr, kGameplayStaticsPath);
    if (!gs_class) {
        log::line(L"synth: GameplayStatics class not resolvable");
        return nullptr;
    }
    RC::Unreal::UObject* cdo = gs_class->GetClassDefaultObject().ObjectPtr;
    if (!cdo) {
        log::line(L"synth: GameplayStatics CDO not resolvable");
        return nullptr;
    }
    RC::Unreal::UFunction* begin = find_ufunction(
        L"/Script/Engine.GameplayStatics:BeginSpawningActorFromClass");
    RC::Unreal::UFunction* finish = find_ufunction(
        L"/Script/Engine.GameplayStatics:FinishSpawningActor");
    if (!begin || !finish) {
        log::line(L"synth: GameplayStatics deferred-spawn functions not resolvable");
        return nullptr;
    }

    // --- BeginSpawningActorFromClass(WorldContextObject, ActorClass,
    //        SpawnTransform, bNoCollisionFail, CollisionHandlingOverride) -> AActor
    void* bbuf = ::operator new(begin->GetParmsSize());
    std::memset(bbuf, 0, begin->GetParmsSize());

    RC::Unreal::FProperty* p_wco =
        begin->FindProperty(RC::Unreal::FName(L"WorldContextObject", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* p_cls =
        begin->FindProperty(RC::Unreal::FName(L"ActorClass", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* p_tr =
        begin->FindProperty(RC::Unreal::FName(L"SpawnTransform", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* p_nc =
        begin->FindProperty(RC::Unreal::FName(L"bNoCollisionFail", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* p_ret =
        begin->FindProperty(RC::Unreal::FName(L"ReturnValue", RC::Unreal::EFindName::FName_Add));

    if (p_wco) std::memcpy(reinterpret_cast<char*>(bbuf) + p_wco->GetOffset_Internal(), &world, sizeof(world));
    if (p_cls) std::memcpy(reinterpret_cast<char*>(bbuf) + p_cls->GetOffset_Internal(), &class_to_spawn, sizeof(class_to_spawn));
    bool no_collision = true;
    if (p_nc) std::memcpy(reinterpret_cast<char*>(bbuf) + p_nc->GetOffset_Internal(), &no_collision, sizeof(no_collision));
    if (p_tr) fill_identity_transform(reinterpret_cast<char*>(bbuf) + p_tr->GetOffset_Internal());

    cdo->ProcessEvent(begin, bbuf);
    RC::Unreal::UObject* pending = nullptr;
    if (p_ret) {
        std::memcpy(&pending, reinterpret_cast<char*>(bbuf) + p_ret->GetOffset_Internal(), sizeof(pending));
    }
    ::operator delete(bbuf);

    if (!pending) {
        log::line(L"synth: BeginSpawningActorFromClass returned null");
        return nullptr;
    }

    // --- FinishSpawningActor(Actor, SpawnTransform, bNoCollisionFail,
    //        CollisionHandlingOverride) -> AActor
    void* fbuf = ::operator new(finish->GetParmsSize());
    std::memset(fbuf, 0, finish->GetParmsSize());
    RC::Unreal::FProperty* f_actor = finish->FindProperty(
        RC::Unreal::FName(L"Actor", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* f_tr = finish->FindProperty(
        RC::Unreal::FName(L"SpawnTransform", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* f_nc = finish->FindProperty(
        RC::Unreal::FName(L"bNoCollisionFail", RC::Unreal::EFindName::FName_Add));
    RC::Unreal::FProperty* f_ret = finish->FindProperty(
        RC::Unreal::FName(L"ReturnValue", RC::Unreal::EFindName::FName_Add));
    if (f_actor) std::memcpy(reinterpret_cast<char*>(fbuf) + f_actor->GetOffset_Internal(), &pending, sizeof(pending));
    if (f_tr) fill_identity_transform(reinterpret_cast<char*>(fbuf) + f_tr->GetOffset_Internal());
    if (f_nc) std::memcpy(reinterpret_cast<char*>(fbuf) + f_nc->GetOffset_Internal(), &no_collision, sizeof(no_collision));

    cdo->ProcessEvent(finish, fbuf);
    RC::Unreal::UObject* result = pending;
    if (f_ret) {
        std::memcpy(&result, reinterpret_cast<char*>(fbuf) + f_ret->GetOffset_Internal(), sizeof(result));
    }
    ::operator delete(fbuf);

    if (!result) {
        log::line(L"synth: FinishSpawningActor returned null");
        return nullptr;
    }
    return result;
}

void possess(RC::Unreal::UObject* controller, RC::Unreal::UObject* pawn)
{
    RC::Unreal::UFunction* possess_fn = controller->GetFunctionByNameInChain(L"Possess");
    if (!possess_fn) {
        log::line(L"synth: Possess not resolvable - controller has no pawn");
        return;
    }
    void* buf = ::operator new(possess_fn->GetParmsSize());
    std::memset(buf, 0, possess_fn->GetParmsSize());
    RC::Unreal::FProperty* inpawn = possess_fn->FindProperty(
        RC::Unreal::FName(L"InPawn", RC::Unreal::EFindName::FName_Add));
    if (inpawn) {
        std::memcpy(reinterpret_cast<char*>(buf) + inpawn->GetOffset_Internal(), &pawn, sizeof(pawn));
    }
    controller->ProcessEvent(possess_fn, buf);
    ::operator delete(buf);
}

void stash_user_profile(RC::Unreal::UObject* controller)
{
    // Non-fatal best-effort. A UserProfile on the
    // controller's _userProfile property for chat-pipeline routing. We only
    // verify presence; constructing a full UserProfile is out of scope for the
    // headless direct-executor path.
    RC::Unreal::UClass* cclass = controller->GetClassPrivate();
    if (!cclass) return;
    RC::Unreal::FProperty* prop = cclass->FindProperty(
        RC::Unreal::FName(L"_userProfile", RC::Unreal::EFindName::FName_Add));
    if (!prop) {
        log::line(L"synth: _userProfile property not found on controller (non-fatal)");
    }
}

} // anonymous namespace

// ===========================================================================
//  find_main_world
// ===========================================================================
RC::Unreal::UObject* find_main_world()
{
    RC::Unreal::UObject* world = nullptr;
    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](RC::Unreal::UObject* obj, int, int) -> RC::LoopAction
    {
        auto* klass = obj->GetClassPrivate();
        if (!klass) return RC::LoopAction::Continue;
        if (klass->GetName() == L"World") {
            if (obj->HasAnyFlags(RC::Unreal::EObjectFlags::RF_ClassDefaultObject))
                return RC::LoopAction::Continue;
            world = obj;
            return RC::LoopAction::Break;
        }
        return RC::LoopAction::Continue;
    });
    return world;
}

// ===========================================================================
//  get_or_build_companion_controller
// ===========================================================================
RC::Unreal::UObject* get_or_build_companion_controller()
{
    std::lock_guard<std::mutex> lk(g_cache.m);

    RC::Unreal::UObject* world = find_main_world();
    if (!world) {
        log::line(L"synth: world not resolvable - cannot build companion");
        return nullptr;
    }

    auto alive = [](RC::Unreal::UObject* o) {
        return o && !o->IsUnreachable();
    };

    if (alive(g_cache.controller) && alive(g_cache.character) &&
        g_cache.world == world) {
        return g_cache.controller;
    }

    RC::Unreal::UClass* ctrl_class = nullptr;
    RC::Unreal::UClass* char_class = nullptr;
    if (!find_classes(&ctrl_class, &char_class)) {
        log::line(L"synth: controller/character class not resolvable");
        return nullptr;
    }

    RC::Unreal::UObject* ctrl = spawn_actor(ctrl_class, world);
    if (!ctrl) {
        log::line(L"synth: controller spawn failed");
        return nullptr;
    }
    RC::Unreal::UObject* chr = spawn_actor(char_class, world);
    if (!chr) {
        log::line(L"synth: character spawn failed - controller has no pawn");
        return nullptr;
    }

    possess(ctrl, chr);
    stash_user_profile(ctrl);

    g_cache.controller = ctrl;
    g_cache.character  = chr;
    g_cache.world      = world;

    log::line(std::wstring(L"synth: controller ready (") + ctrl->GetFullName() +
              L", pawn " + chr->GetFullName() + L")");
    return ctrl;
}

} // namespace scum_rcon::synth
