// RconMod.h -- main mod class declaration
//
// Only on_update and on_unreal_init are overridden; all other lifecycle slots
// resolve to the imported base no-ops from UE4SS.dll.
#pragma once
#include <UE4SS.h>

namespace scum_rcon {

class RconMod : public RC::CppUserModBase {
public:
    RconMod();
    ~RconMod() override;

    void on_unreal_init() override;
    void on_update() override;
};

} // namespace scum_rcon