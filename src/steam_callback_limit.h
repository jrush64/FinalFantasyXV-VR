#pragma once
#include <windows.h>
#include <atomic>
#include <cwchar>
#include <cstdio>
#include "MinHook.h"

// Independently implemented from the public Steam callback API contract.
// Only the callback pump is hooked. Live testing must verify dependent Steam behavior.
namespace SteamCallbackLimit {
using Function = void (__cdecl*)();
using Logger = void (*)(const char*);
struct Gate {
    std::atomic<long long> last{-1};
    bool Admit(long long now, long long interval) {
        if(interval <= 0) return true;
        auto previous = last.load(std::memory_order_relaxed);
        for (;;) {
            if(previous >= 0 && now >= previous && now-previous < interval) return false;
            if(last.compare_exchange_weak(previous, now, std::memory_order_relaxed)) return true;
        }
    }
};
inline Gate gate;
inline Function original{}, releaseMemory{};
inline Logger logger{};
inline long long interval{};
inline std::atomic_flag reportedFirst = ATOMIC_FLAG_INIT;
inline std::atomic_flag reportedSkip = ATOMIC_FLAG_INIT;
inline void __cdecl RunCallbacks() {
    LARGE_INTEGER now{};
    const bool allowed = !QueryPerformanceCounter(&now) || gate.Admit(now.QuadPart, interval);
    if(allowed) {
        original();
        if(!reportedFirst.test_and_set() && logger)
            logger("[STEAMCALLBACK] first original callback pump completed; caller thread retained");
    } else {
        // RunCallbacks normally performs this cleanup. Preserve it on skipped calls.
        releaseMemory();
        if(!reportedSkip.test_and_set() && logger)
            logger("[STEAMCALLBACK] redundant callback pump suppressed; thread-memory cleanup retained");
    }
}
inline bool Install(HMODULE module, int hz, Logger log) {
    logger = log;
    if(hz == 0) { if(log)log("[STEAMCALLBACK] disabled by configuration"); return false; }
    if(hz < 10 || hz > 60) { if(log)log("[STEAMCALLBACK] invalid rate; unchanged (allowed: 0 or 10..60)"); return false; }
    auto target = module ? GetProcAddress(module,"SteamAPI_RunCallbacks") : nullptr;
    auto cleanup = module ? GetProcAddress(module,"SteamAPI_ReleaseCurrentThreadMemory") : nullptr;
    LARGE_INTEGER frequency{};
    if(!target || !cleanup || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        if(log)log("[STEAMCALLBACK] required exports or clock unavailable; unchanged"); return false;
    }
    const auto init = MH_Initialize();
    if(init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        if(log)log("[STEAMCALLBACK] hook initialization failed; unchanged"); return false;
    }
    releaseMemory = reinterpret_cast<Function>(cleanup);
    interval = (frequency.QuadPart + hz - 1) / hz;
    if(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&RunCallbacks),
        reinterpret_cast<void**>(&original)) != MH_OK) {
        if(log)log("[STEAMCALLBACK] export already hooked or unsupported; unchanged"); return false;
    }
    if(MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        if(log)log("[STEAMCALLBACK] hook activation failed; unchanged"); return false;
    }
    char line[160]{};
    sprintf_s(line,"[STEAMCALLBACK] installed limit=%d Hz; no sleeps, worker thread, or frame limiter",hz);
    if(log)log(line);
    return true;
}
inline void Start(HMODULE self, Logger log) {
    wchar_t path[MAX_PATH]{};
    auto length = GetModuleFileNameW(self,path,MAX_PATH);
    if(!length || length >= MAX_PATH) return;
    auto slash = wcsrchr(path,L'\\');
    if(!slash || wcscpy_s(slash+1, MAX_PATH-(slash+1-path),L"ffxv-vr.ini")) return;
    const int hz = static_cast<int>(GetPrivateProfileIntW(L"Performance",L"SteamCallbacksHz",0,path));
    if(hz == 0) { Install(nullptr,0,log); return; }
    HMODULE steam{};
    // Retain the loaded module; never load another Steam API or replace its DLL.
    GetModuleHandleExW(0,L"steam_api64.dll",&steam);
    Install(steam,hz,log);
}
}
