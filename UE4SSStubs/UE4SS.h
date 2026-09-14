// UE4SS.h -- self-contained ABI stub for the installed UE4SS.dll
//
// This header declares ONLY symbols that UE4SS.dll exports, with the EXACT
// signatures verified against the 4069-entry export list dumped from the
// installed E:\Win64 (1)\ue4ss\UE4SS.dll. Every symbol referenced below must
// mangle to a name present in the import library generated from that DLL.
//
// Rules used while writing this file:
//   * __declspec(dllimport) functions are declaration-only (no bodies, no
//     "= default"): a body with dllimport is error C2491.
//   * Layout-critical ABI facts:
//       - CppUserModBase : vptr(8) + GUITabs vector(24) + 5x std::wstring(160)
//                          = 192 bytes (0xC0), 16 virtuals in UE4SS order.
//       - FCallbackOptions : passed BY VALUE to RegisterEngineTickPreCallback /
//                            RegisterProcessEventPreCallback, so it must be
//                            exactly {bool, bool, wstring, wstring} (72 bytes).
//       - UnrealScriptFunctionCallableContext : {UObject* Context;
//                            FFrame& TheStack; void* Result;} = 24 bytes.
//       - FName  : 8 bytes {uint32 ComparisonIndex; uint32 Number;}
//       - FString: 16 bytes {wchar_t* Data; int32 ArrayNum; int32 ArrayMax;}
//       - TObjectPtr<T> : 8 bytes, single raw pointer member (UE 4.27 layout).
//   * Only the symbols actually used by the port are declared. Symbols that
//     would require full UE4SS containers (TArray, TMap, TFieldRange,
//     FFieldClassVariant) are intentionally omitted.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#define RC_UE4SS_API __declspec(dllimport)

namespace RC
{
    using StringType     = std::wstring;
    using StringViewType = std::wstring_view;

    enum class LoopAction
    {
        Continue,
        Break,
        Stop,
    };

    namespace LuaMadeSimple
    {
        class Lua;
    }

    namespace GUI
    {
        class GUITab;
    }

    struct UE4SSRuntime
    {
        RC_UE4SS_API static auto IsEngineTickAvailable() -> bool;
        RC_UE4SS_API static auto IsProcessEventAvailable() -> bool;
    };

    class CppUserModBase
    {
      protected:
        std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

      public:
        StringType ModName{};
        StringType ModVersion{};
        StringType ModDescription{};
        StringType ModAuthors{};
        StringType ModIntendedSDKVersion{};

      public:
        RC_UE4SS_API CppUserModBase();
        RC_UE4SS_API virtual ~CppUserModBase();

      public:
        // WARNING: the vtable slot order below must match UE4SS's
        // CppUserModBase declaration EXACTLY (verified against the original
        // main.dll's vftable). UE4SS dispatches lifecycle calls by slot index.
        RC_UE4SS_API virtual auto on_update() -> void;
        RC_UE4SS_API virtual auto on_unreal_init() -> void;
        RC_UE4SS_API virtual auto on_ui_init() -> void;
        RC_UE4SS_API virtual auto on_program_start() -> void;
        // MSVC emits overloaded virtuals into the vtable in REVERSE
        // declaration order, so these are listed last-to-first to land on the
        // same slots as the original (Lua*, Lua*, vector, vector).
        RC_UE4SS_API virtual auto on_lua_start(StringViewType,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               std::vector<LuaMadeSimple::Lua*>&) -> void;
        RC_UE4SS_API virtual auto on_lua_start(LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               std::vector<LuaMadeSimple::Lua*>&) -> void;
        RC_UE4SS_API virtual auto on_lua_start(StringViewType,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua*) -> void;
        RC_UE4SS_API virtual auto on_lua_start(LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua&,
                                               LuaMadeSimple::Lua*) -> void;
        RC_UE4SS_API virtual auto on_lua_stop(StringViewType,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              std::vector<LuaMadeSimple::Lua*>&) -> void;
        RC_UE4SS_API virtual auto on_lua_stop(LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              std::vector<LuaMadeSimple::Lua*>&) -> void;
        RC_UE4SS_API virtual auto on_lua_stop(StringViewType,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua*) -> void;
        RC_UE4SS_API virtual auto on_lua_stop(LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua&,
                                              LuaMadeSimple::Lua*) -> void;
        RC_UE4SS_API virtual auto on_dll_load(StringViewType) -> void;
        RC_UE4SS_API virtual auto render_tab() -> void;
        RC_UE4SS_API virtual auto on_cpp_mods_loaded() -> void;
    };

    namespace Output
    {
        RC_UE4SS_API auto send(StringViewType str) -> void;
        RC_UE4SS_API auto has_internal_error() -> bool;
    } // namespace Output

    namespace Unreal
    {
        // --- Forward declarations (never laid out) ---
        struct FFrame;
        struct FOutParmRec;
        struct ObjectSearcher;
        struct FStaticConstructObjectParameters;
        class UObjectBase;
        class UObject;
        class UField;
        class UStruct;
        class UFunction;
        class UClass;
        class UEngine;
        class UWorld;
        class AActor;
        class FField;
        class FProperty;
        class FName;
        class FString;

        template <typename T>
        class TObjectPtr
        {
          public:
            T* ObjectPtr = nullptr;
        };

        // --- Enums (values match UE4SS / UE) ---
        enum class EFindName : int
        {
            FName_None = 0,
            FName_Add = 1,
            FName_Replace_Not_Safe_For_Threading = 2,
            FName_Replace_Not_Safe_For_Threading_No_Add = 3,
            FName_Find = 4,
        };

        enum class EClassCastFlags : std::uint64_t
        {
            None = 0x0000000000000000ull,
            UField = 0x0000000000000001ull,
            UStruct = 0x0000000000000002ull,
            UClass = 0x0000000000000004ull,
            UFunction = 0x0000000000000008ull,
            AActor = 0x0000000000000010ull,
            UWorld = 0x0000000000000020ull,
            AController = 0x0000000000000040ull,
            APlayerController = 0x0000000000000080ull,
        };

        enum class EObjectFlags : std::int32_t
        {
            RF_NoFlags = 0x00000000,
            RF_Public = 0x00000001,
            RF_Standalone = 0x00000002,
            RF_Transient = 0x00000008,
            RF_ClassDefaultObject = 0x00000010,
            RF_ArchetypeObject = 0x00000020,
        };

        enum class EPropertyFlags : std::uint64_t
        {
            CPF_None = 0,
            CPF_Edit = 0x0000000000000001ull,
            CPF_BlueprintVisible = 0x0000000000000010ull,
            CPF_Parm = 0x0000000000000080ull,
            CPF_OutParm = 0x0000000000000100ull,
            CPF_ReturnParm = 0x0000000000000400ull,
        };

        // --- FName (8 bytes) ---
        class FName
        {
          public:
            std::uint32_t ComparisonIndex = 0;
            std::uint32_t Number = 0;

            RC_UE4SS_API FName();
            RC_UE4SS_API FName(std::uint32_t InComparisonIndex, std::uint32_t InNumber);
            RC_UE4SS_API FName(std::int64_t packed);
            RC_UE4SS_API FName(const wchar_t* Name, EFindName FindType, void* Data = nullptr);
            RC_UE4SS_API FName(std::wstring_view Name, EFindName FindType, void* Data = nullptr);
            RC_UE4SS_API FName(std::wstring_view Name,
                               std::uint32_t Number,
                               EFindName FindType,
                               void* Data = nullptr);

            FName(const FName&) = default;
            FName(FName&&) = default;
            FName& operator=(const FName&) = default;
            FName& operator=(FName&&) = default;

            RC_UE4SS_API auto ToString() const -> const std::wstring;
        };

        // --- FString (16 bytes) ---
        class FString
        {
          public:
            wchar_t* Data = nullptr;
            std::int32_t ArrayNum = 0;
            std::int32_t ArrayMax = 0;

          public:
            RC_UE4SS_API FString();
            RC_UE4SS_API FString(const wchar_t* Other);
            RC_UE4SS_API FString(wchar_t* Other);
            RC_UE4SS_API FString(const std::wstring& Other);
            RC_UE4SS_API FString(const FString& Other);
            RC_UE4SS_API FString& operator=(const FString& Other);
            RC_UE4SS_API ~FString();

            RC_UE4SS_API auto Clear() -> void;

            auto wstring() const -> std::wstring
            {
                if (!Data || ArrayNum <= 0) return {};
                return std::wstring(Data, static_cast<std::size_t>(ArrayNum));
            }
        };

        // --- FFrame (opaque; exported accessors only) ---
        struct FFrame
        {
            RC_UE4SS_API auto Locals() -> std::uint8_t*&;
            RC_UE4SS_API auto OutParms() -> FOutParmRec*&;
            RC_UE4SS_API auto Node() -> UFunction*&;
            RC_UE4SS_API auto Object() -> UObject*&;
            RC_UE4SS_API auto Code() -> std::uint8_t*&;
            RC_UE4SS_API auto MostRecentProperty() -> FProperty*&;
            RC_UE4SS_API auto MostRecentPropertyAddress() -> std::uint8_t*&;
            RC_UE4SS_API auto MostRecentPropertyContainer() -> std::uint8_t*&;
            RC_UE4SS_API auto Step(UObject* Context, void* Result) -> void;
        };

        // --- UnrealScriptFunctionCallableContext (24 bytes) ---
        class UnrealScriptFunctionCallableContext
        {
          public:
            UObject* Context;
            FFrame& TheStack;
            void* Result;
        };

        // --- Hook namespace ---
        namespace Hook
        {
            struct FCallbackOptions
            {
                bool bRunOnGameThread;
                bool bPassContext;
                StringType DebugName;
                StringType DebugTag;

                RC_UE4SS_API FCallbackOptions();
                RC_UE4SS_API FCallbackOptions(const FCallbackOptions&);
                RC_UE4SS_API ~FCallbackOptions();
            };

            template <typename T>
            class TCallbackIterationData;

            RC_UE4SS_API auto RegisterEngineTickPreCallback(
                const std::function<void(TCallbackIterationData<void>&, UEngine*, float, bool)> callback,
                FCallbackOptions options) -> std::uint64_t;

            RC_UE4SS_API auto RegisterProcessEventPreCallback(
                const std::function<void(TCallbackIterationData<void>&, UObject*, UFunction*, void*)> callback,
                FCallbackOptions options) -> std::uint64_t;

            RC_UE4SS_API auto UnregisterCallback(std::uint64_t handle) -> bool;
        } // namespace Hook

        // --- UObject hierarchy (no layout; only exported accessors) ---
        class UObjectBase
        {
          public:
            RC_UE4SS_API auto GetClassPrivate() -> UClass*&;
            RC_UE4SS_API auto GetInternalIndex() -> int;
            RC_UE4SS_API auto IsA(UClass* SomeBase) const -> bool;
            RC_UE4SS_API auto IsA(const UClass* SomeBase) const -> bool;
        };

        class UObject : public UObjectBase
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;

            RC_UE4SS_API auto GetFName() const -> FName;
            RC_UE4SS_API auto GetName() const -> std::wstring;
            RC_UE4SS_API auto GetFullName(UObject* FullName = nullptr) const -> std::wstring;
            RC_UE4SS_API auto GetWorld() const -> UWorld*;
            RC_UE4SS_API auto GetFunctionByName(const wchar_t* FunctionName) -> UFunction*;
            RC_UE4SS_API auto GetFunctionByName(FName FunctionName) -> UFunction*;
            RC_UE4SS_API auto GetFunctionByNameInChain(const wchar_t* FunctionName) -> UFunction*;
            RC_UE4SS_API auto ProcessEvent(UFunction* Function, void* Params) -> void;
            RC_UE4SS_API auto HasAnyFlags(EObjectFlags FlagsToCheck) -> bool;
            RC_UE4SS_API auto SetFlags(EObjectFlags NewFlags) -> void;
            RC_UE4SS_API auto IsUnreachable() -> bool;
        };

        class UField : public UObject
        {
          public:
            RC_UE4SS_API auto GetNext() -> TObjectPtr<UField>&;
        };

        class UStruct : public UField
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;

            RC_UE4SS_API auto GetSuperStruct() -> UStruct*&;
            RC_UE4SS_API auto GetSuperStruct() const -> const UStruct*&;
            RC_UE4SS_API auto GetChildProperties() -> FField*&;
            RC_UE4SS_API auto GetChildren() -> TObjectPtr<UField>&;
            RC_UE4SS_API auto GetStructureSize() const -> int;
            RC_UE4SS_API auto FindProperty(const FName Name) -> FProperty*;
            RC_UE4SS_API auto CustomFindProperty(const FName Name) const -> FProperty*;
            RC_UE4SS_API auto IsChildOf(UStruct* SomeBase) -> bool;
            RC_UE4SS_API auto IsChildOf(const UStruct* SomeBase) const -> bool;
        };

        class UFunction : public UStruct
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;

            RC_UE4SS_API auto GetFuncPtr() -> void (*)(UObject*, FFrame&, void*);
            RC_UE4SS_API auto GetNumParms() -> std::uint8_t&;
            RC_UE4SS_API auto GetNumParms() const -> const std::uint8_t&;
            RC_UE4SS_API auto GetParmsSize() -> std::uint16_t&;
            RC_UE4SS_API auto GetParmsSize() const -> const std::uint16_t&;
            RC_UE4SS_API auto GetReturnValueOffset() -> std::uint16_t&;
            RC_UE4SS_API auto GetReturnValueOffset() const -> const std::uint16_t&;
        };

        class UClass : public UStruct
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;

            RC_UE4SS_API auto GetClassCastFlags() -> std::uint64_t&;
            RC_UE4SS_API auto GetClassCastFlags() const -> const std::uint64_t&;
            RC_UE4SS_API auto HasAllCastFlags(EClassCastFlags FlagsToCheck) const -> bool;
            RC_UE4SS_API auto GetClassDefaultObject() -> TObjectPtr<UObject>&;
            RC_UE4SS_API auto GetClassDefaultObject() const -> const TObjectPtr<UObject>&;
        };

        class UEngine : public UObject
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;
        };

        class UWorld : public UObject
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;
        };

        class AActor : public UObject
        {
          public:
            RC_UE4SS_API static auto StaticClass() -> UClass*;
        };

        // --- FField / FProperty ---
        class FField
        {
          public:
            RC_UE4SS_API auto GetFName() -> FName;
            RC_UE4SS_API auto GetName() -> std::wstring;
            // Inline-доступ к приватному экспортированному GetNext()
            // (decorated name ?GetNext@FField@...@@AEAA... - private member).
            auto NextField() -> FField*& { return GetNext(); }
          private:
            RC_UE4SS_API auto GetNext() -> FField*&;
        };

        class FProperty : public FField
        {
          public:
            RC_UE4SS_API auto GetOffset_Internal() -> int&;
            RC_UE4SS_API auto GetOffset_Internal() const -> const int&;
            RC_UE4SS_API auto GetSize() -> int;
            RC_UE4SS_API auto GetSize() const -> int;
            RC_UE4SS_API auto GetElementSize() -> int&;
            RC_UE4SS_API auto GetElementSize() const -> const int&;
            RC_UE4SS_API auto GetMinAlignment() const -> int;
            RC_UE4SS_API auto HasAllPropertyFlags(EPropertyFlags FlagsToCheck) -> bool;
            RC_UE4SS_API auto InitializeValue(void* Dest) const -> void;
            RC_UE4SS_API auto DestroyValue(void* Dest) const -> void;
            RC_UE4SS_API auto AllocateAndInitializeValue() const -> void*;
            RC_UE4SS_API auto CopyCompleteValue(void* Dest, const void* Src) const -> void;
        };

        // --- UObjectGlobals ---
        namespace UObjectGlobals
        {
            RC_UE4SS_API auto FindObject(UClass* ObjectClass,
                                         UObject* Outer = nullptr,
                                         const wchar_t* ObjectName = nullptr,
                                         bool bExactClass = false,
                                         ObjectSearcher* Searcher = nullptr) -> UObject*;

            RC_UE4SS_API auto ForEachUObject(
                const std::function<LoopAction(UObject*, int, int)>& callback) -> void;

            RC_UE4SS_API auto RegisterHook(
                const std::wstring& FunctionName,
                const std::function<void(UnrealScriptFunctionCallableContext&, void*)> PreCallback,
                const std::function<void(UnrealScriptFunctionCallableContext&, void*)> PostCallback,
                void* CustomData) -> std::pair<int, int>;

            RC_UE4SS_API auto RegisterHook(
                UFunction* Function,
                const std::function<void(UnrealScriptFunctionCallableContext&, void*)> PreCallback,
                const std::function<void(UnrealScriptFunctionCallableContext&, void*)> PostCallback,
                void* CustomData) -> std::pair<int, int>;

            RC_UE4SS_API auto UnregisterHook(const std::wstring& FunctionName,
                                             std::pair<int, int> hook_ids) -> void;

            RC_UE4SS_API auto UnregisterHook(UFunction* Function,
                                             std::pair<int, int> hook_ids) -> void;

            RC_UE4SS_API auto StaticConstructObject(
                const FStaticConstructObjectParameters& Parameters) -> UObject*;

template <typename T, typename... Args>
            static auto FindObject(Args... args) -> T*
            {
                return static_cast<T*>(FindObject(T::StaticClass(), args...));
            }
        } // namespace UObjectGlobals

        // --- Free helpers ---
        RC_UE4SS_API auto IsInGameThreadRaw() -> bool;

        // --- Version ---
        namespace Version
        {
            RC_UE4SS_API extern int Major;
            RC_UE4SS_API extern int Minor;
        }
    } // namespace Unreal
} // namespace RC
