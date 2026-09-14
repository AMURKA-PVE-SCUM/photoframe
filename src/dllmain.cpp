// dllmain.cpp -- DLL exports consumed by UE4SS (start_mod / uninstall_mod)
//
// start_mod      : operator new(sizeof(RconMod)) then the ctor
// uninstall_mod  : delete the RconMod* (scalar-deleting destructor)
#include "RconMod.h"
#include "scum_rcon.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD fdwReason, LPVOID)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
void* start_mod()
{
    return new scum_rcon::RconMod();
}

extern "C" __declspec(dllexport)
void uninstall_mod(void* mod)
{
    if (!mod) return;
    delete static_cast<scum_rcon::RconMod*>(mod);
}