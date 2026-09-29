#pragma once

#include <d3d11.h>

namespace SceneCensus
{
using LogFn = void (*)(const char*, ...);
using ReadOutputFn = int (*)();

void Install(ID3D11Device* device, ID3D11DeviceContext* immediate,
    ID3D11DeviceContext* deferred, LogFn log, ReadOutputFn readOutput);
void BeginCapture(bool stereo);
void OnPresent(bool stereo);
bool IsCapturing();
// [UIFIT] draws whose viewport was pulled inward this session.
unsigned long long GetUiFitDrawCount();
// [PASSDRAW] The engine announces each render pass by name.  Feed those in and
// every draw is attributed to the pass that was last announced, which is how
// the interface gets identified without guessing from D3D state.
void SetCurrentPass(const char* name);
void OnPassDrawFrame();
// Install the census hooks on a context discovered at runtime.  Idempotent and
// keyed by vtable, so it is safe to offer the same context repeatedly.
bool EnsureContextHooked(ID3D11DeviceContext* context);
}
