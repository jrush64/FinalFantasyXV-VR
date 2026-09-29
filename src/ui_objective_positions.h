#pragma once
#include "ui_objective_world.h"
#include "ui_world_probe.h"
namespace ObjectivePositions {
inline SRWLOCK lock=SRWLOCK_INIT;
inline ObjectiveWorld::History history;
inline UiWorldProbe::ProjectFn original{};
inline bool installed{};
inline std::uintptr_t __fastcall Project(std::uint32_t scene,const float* input,float* output,std::uint32_t camera) {
    float world[4]{};
    const bool read=scene==0 && camera==0 && input && UiWorldProbe::Read(world,input,sizeof(world));
    const auto result=UiWorldProbe::Sampling() ? UiWorldProbe::Invoke(scene,input,output,camera,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress())) : original(scene,input,output,camera);
    float projected[4]{};
    if(read && (result&0xff)==1 && output && UiWorldProbe::Read(projected,output,sizeof(projected)) &&
       projected[3]>.1f && std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]) &&
       std::isfinite(projected[0]) && std::isfinite(projected[1]) && TryAcquireSRWLockExclusive(&lock)) {
        history.Add({reinterpret_cast<std::uint64_t>(output),GetTickCount64(),
            {world[0],world[1],world[2]},{projected[0],projected[1]}});
        ReleaseSRWLockExclusive(&lock);
    }
    return result;
}
inline bool Find(float x,float y,ObjectiveWorld::Anchor& a,float yTolerance=128) {
    if(!TryAcquireSRWLockShared(&lock)) return false;
    const bool found=history.Find(x,y,GetTickCount64(),a,yTolerance);
    ReleaseSRWLockShared(&lock); return found;
}
inline void Install(UiWorldProbe::LogFn log) {
    const auto module=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(!UiWorldProbe::VerifyImage(module)) {if(log)log("[OBJECTIVE3D] position hook refused: unexpected executable");return;}
    auto target=reinterpret_cast<void*>(module+0x564220);
    auto status=MH_CreateHook(target,reinterpret_cast<void*>(&Project),reinterpret_cast<void**>(&original));
    if(status==MH_OK) {
        status=MH_EnableHook(target);
        if(status!=MH_OK) MH_RemoveHook(target);
    }
    installed=status==MH_OK;
    if(installed) { UiWorldProbe::original=original;UiWorldProbe::Install(log,true); }
    if(log)log("[OBJECTIVE3D] guarded scene-0 position provider %s; objective-only world billboard",MH_StatusToString(status));
}
}
