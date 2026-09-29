#pragma once
#include <dxgi1_6.h>
namespace DesktopPresentPolicy {
inline bool Flip(DXGI_SWAP_EFFECT effect) {return effect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect==DXGI_SWAP_EFFECT_FLIP_DISCARD;}
inline UINT CreationFlags(IDXGIFactory* factory, DXGI_SWAP_EFFECT effect, bool windowed, UINT flags) {
    if(!factory || !windowed || !Flip(effect)) return flags;
    IDXGIFactory5* f{}; BOOL supported=FALSE;
    if(SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),reinterpret_cast<void**>(&f)))) {
        if(FAILED(f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported)))) supported=FALSE;
        f->Release();
    }
    return supported ? flags|DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : flags;
}
inline UINT ResizeFlags(IDXGISwapChain* chain, UINT flags) {
    DXGI_SWAP_CHAIN_DESC d{};
    if(!chain || FAILED(chain->GetDesc(&d))) return flags;
    // This creation-time bit must remain identical across both resize APIs.
    return (flags&~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)|(d.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
}
inline UINT PresentFlags(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if(!chain || sync || flags) return flags; // preserve tests/special present operations
    DXGI_SWAP_CHAIN_DESC d{}; BOOL exclusive=TRUE;
    if(FAILED(chain->GetDesc(&d)) || !(d.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) || !Flip(d.SwapEffect)) return flags;
    if(FAILED(chain->GetFullscreenState(&exclusive,nullptr)) || exclusive) return flags;
    return flags|DXGI_PRESENT_ALLOW_TEARING;
}
template<class Call>
HRESULT Present(Call call, UINT original, UINT adjusted, bool* fallback=nullptr) {
    const auto result=call(adjusted);
    // Rejected presentation has not advanced the swapchain. Preserve operation
    // if a fullscreen transition races the state check; never hide device errors.
    if(result==DXGI_ERROR_INVALID_CALL && adjusted!=original) {if(fallback)*fallback=true;return call(original);}
    return result;
}
}
