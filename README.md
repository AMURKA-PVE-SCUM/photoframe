# SCUM-RCON (UE4SS C++ port)

Port of the original SCUM-RCON `main.dll` mod to a clean-room C++ implementation
built against the installed `UE4SS.dll` — no UE SDK, no leaked source. The
original binary was analyzed (imports, vtable layout, function signatures) so the
port reproduces its exact ABI.

## What it does

* Hosts a [Source RCON](https://developer.valvesoftware.com/wiki/Source_RCON)
  server (`bind_address`/`port` from `config.ini`).
* Routes RCON commands to in-game `AdminCommand` subclasses (verb map built by
  scanning the `UClass` tree for subclasses of `/Script/SCUM.AdminCommand`).
* Provides C++ verbs that do not map to a game command:
  * `SendChat <type 0-7> "<message>" [<17-digit SteamID>]` — chat line via
    `/Script/SCUM.MiscStatics:SendChatLineToPlayer` (no SteamID = broadcast).
  * `ListSquads` — squads from the local SQLite cache.
  * `FindQuestLockouts` — players holding a blocked quest (login-lockout).
  * `RunQuestUnstick` — delete the offending `active_quest` rows.
  * `DeleteActiveQuestsForUser <17-digit SteamID>` — delete one player's rows.
  * `Unstuck` — placeholder (use the in-game `/unstuck` command instead).
* Quest-lockout recovery uses a local SQLite database (bundled amalgamation)
  mirroring the game's quest tables.
* Runs on the game thread (clean-frame path via the EngineTick pre-callback, or
  a ProcessEvent pre-callback fallback) so `ProcessEvent` calls are safe.

## Safety

The RCON listener will **not start** until `password` in `config.ini` is changed
from `CHANGE_ME_BEFORE_USE` (fail-closed). Keep the port closed to the public
network: the RCON protocol here is not encrypted.

## Layout

```
port/
  UE4SSStubs/UE4SS.h   stub headers matching the installed UE4SS.dll ABI
  src/                 all mod sources (server, dispatch, hooks, questdb, config)
  build.ps1            build script (MSVC Build Tools)
  build/               build output (main.dll)
ue4ss/
  Mods/scum_rcon/      deployment target
    config.ini
    enabled.txt
    dlls/main.dll
```

## Build prerequisites

* Visual Studio 2022 Build Tools (`vcvars64.bat`, cl/link).
* `UE4SS.lib` — an import library generated from the installed
  `ue4ss\UE4SS.dll` exports (expected at `%TEMP%\opencode\build\UE4SS.lib`, or
  `port\build\UE4SS.lib`).

## Build

```
powershell -ExecutionPolicy Bypass -File "E:\Win64 (1)\port\build.ps1"
```

Produces `port\build\main.dll`.

## Deploy

Copy `port\build\main.dll` to `E:\Win64 (1)\ue4ss\Mods\scum_rcon\dlls\main.dll`
(backing up the original first). `enabled.txt` must be present (it already is).

## Verify the binary is ABI-identical to the original

The port is validated against the original `main.dll`:

* `RconMod` is allocated with `operator new(0xC0)` (192 bytes, same as the
  original) — checked in `start_mod`.
* The `CppUserModBase` vtable slots match the original exactly: slot 0 dtor,
  slot 1 `on_update`, slot 2 `on_unreal_init`, all remaining slots resolving to
  the UE4SS base implementation (import thunks). Note MSVC emits overloaded
  virtuals in *reverse* declaration order — the stub is declared accordingly.
* Import table is byte-identical: same 206 imports (UE4SS.dll + ws2_32 +
  kernel32); no CRT dependency (`/MT`).

## Configuration (`ue4ss\Mods\scum_rcon\config.ini`)

```ini
[rcon]
bind_address = 127.0.0.1
port         = 28015
password     = CHANGE_ME_BEFORE_USE   ; REQUIRED: change before the listener starts
auth_log     = true

[quests]
blocked                =               ; e.g. T3_DC_Interact_ScanAbandonedCity
auto_unstick           = false         ; delete blocked-quest rows at boot
auto_unstick_dry_run   = false
auto_unstick_max_delete = 100          ; circuit breaker

[logging]
verbose = false
```

## In-game verification

1. Set a real `password` in `config.ini`.
2. Confirm `ue4ss\UE4SS-settings.ini` has `HookEngineTick = 1` and
   `HookUObjectProcessEvent = 1` (engine hooks) so the game-thread drain runs.
3. Launch the server, watch the UE4SS console for `[SCUM-RCON]` startup lines.
4. From an RCON client: `login <password>`, then e.g. `SendChat 3 "hello"` or
   any `AdminCommand` verb.

## Notes / limitations

* Runtime behavior of individual SCUM functions (param layout of
  `SendChatLineToPlayer`, the `Execute` command path, quest DB schema) is
  verified only by in-game testing.
* `Unstuck` is intentionally a no-op stub in this build; use the in-game
  `/unstuck` command.
