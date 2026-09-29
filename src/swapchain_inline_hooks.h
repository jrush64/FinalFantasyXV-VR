#pragma once
#include <windows.h>
#include <dxgi1_6.h>
#include <MinHook.h>
#include <array>
#include <atomic>

// Leave the public vtable intact. An overlay discovering a later swapchain
// must never mistake the mod's callback for DXGI's entry point. Each detour calls
// its own executable trampoline, not the potentially patched DXGI address.
namespace SwapchainInline {
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using PresentWork = HRESULT(*)(Present,IDXGISwapChain*,UINT,UINT);
using Present1Work = HRESULT(*)(Present1,IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using ResizeWork = HRESULT(*)(Resize,IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using Logger = void(*)(const char*,...);
inline PresentWork presentWork{};
inline Present1Work present1Work{};
inline ResizeWork resizeWork{};
inline Logger log{};
inline SRWLOCK lock=SRWLOCK_INIT;
inline thread_local unsigned depth{};
inline std::atomic_uint64_t completed{},tests{},nested{};
struct Scope { unsigned previous; Scope():previous(depth++){} ~Scope(){--depth;} };
template<class Fn> struct Route { void* target{}; std::atomic<Fn> next{}; bool enabled{}; };
template<class Fn> inline std::array<Route<Fn>,8> routes{};

template<unsigned I> HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* c,UINT sync,UINT flags){
    const auto next=routes<Present>[I].next.load(std::memory_order_acquire);
    if(!next)return E_UNEXPECTED;
    Scope scope;
    if(flags&DXGI_PRESENT_TEST){++tests;return next(c,sync,flags);}
    if(scope.previous){++nested;return next(c,sync,flags);}
    const auto hr=presentWork?presentWork(next,c,sync,flags):next(c,sync,flags);
    const auto n=++completed;
    if(log && (n<=3 || n%600==0))log("[PRESENTCHAIN] returned=%llu hr=0x%08X testPassThrough=%llu nestedPassThrough=%llu",n,unsigned(hr),tests.load(),nested.load());
    return hr;
}
template<unsigned I> HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* c,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p){
    const auto next=routes<Present1>[I].next.load(std::memory_order_acquire);
    if(!next)return E_UNEXPECTED;
    Scope scope;
    if(flags&DXGI_PRESENT_TEST){++tests;return next(c,sync,flags,p);}
    if(scope.previous){++nested;return next(c,sync,flags,p);}
    return present1Work?present1Work(next,c,sync,flags,p):next(c,sync,flags,p);
}
template<unsigned I> HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags){
    const auto next=routes<Resize>[I].next.load(std::memory_order_acquire);
    if(!next)return E_UNEXPECTED;
    Scope scope;
    return !scope.previous && resizeWork?resizeWork(next,c,n,w,h,f,flags):next(c,n,w,h,f,flags);
}
inline constexpr Present presentHooks[]{PresentHook<0>,PresentHook<1>,PresentHook<2>,PresentHook<3>,PresentHook<4>,PresentHook<5>,PresentHook<6>,PresentHook<7>};
inline constexpr Present1 present1Hooks[]{Present1Hook<0>,Present1Hook<1>,Present1Hook<2>,Present1Hook<3>,Present1Hook<4>,Present1Hook<5>,Present1Hook<6>,Present1Hook<7>};
inline constexpr Resize resizeHooks[]{ResizeHook<0>,ResizeHook<1>,ResizeHook<2>,ResizeHook<3>,ResizeHook<4>,ResizeHook<5>,ResizeHook<6>,ResizeHook<7>};

template<class Fn> bool InstallOne(void* target,const Fn* hooks,const char* name){
    if(!target)return false;
    auto& entries=routes<Fn>;
    for(auto& r:entries)if(r.target==target)return r.enabled;
    for(unsigned i=0;i<entries.size();++i){
        auto& r=entries[i];if(r.target)continue;
        void* trampoline{};
        const auto created=MH_CreateHook(target,reinterpret_cast<void*>(hooks[i]),&trampoline);
        if(created!=MH_OK){if(log)log("[PRESENTCHAIN] %s create failed: %s",name,MH_StatusToString(created));return false;}
        r.target=target;r.next.store(reinterpret_cast<Fn>(trampoline),std::memory_order_release);
        const auto enabled=MH_EnableHook(target);
        r.enabled=enabled==MH_OK;
        if(log)log("[PRESENTCHAIN] %s target=%p trampoline=%p slot unchanged; %s",name,target,trampoline,MH_StatusToString(enabled));
        return r.enabled;
    }
    if(log)log("[PRESENTCHAIN] %s target registry full",name);
    return false;
}
inline bool Install(void* p,void* p1,void* resize,PresentWork work,Present1Work work1,ResizeWork resizeFn,Logger logger){
    AcquireSRWLockExclusive(&lock);
    // The callback identities are fixed for the lifetime of this module.
    if(!presentWork){presentWork=work;present1Work=work1;resizeWork=resizeFn;log=logger;}
    const auto initialized=MH_Initialize();
    bool ok=initialized==MH_OK || initialized==MH_ERROR_ALREADY_INITIALIZED;
    if(ok){
        const bool a=InstallOne(p,presentHooks,"Present");
        const bool b=!p1 || InstallOne(p1,present1Hooks,"Present1");
        const bool c=!resize || InstallOne(resize,resizeHooks,"ResizeBuffers");
        ok=a&&b&&c;
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
}
