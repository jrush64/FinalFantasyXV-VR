#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include "ui_objective_world.h"

// [D3DHOOK] Inline hooks on the d3d11.dll draw implementations themselves.
//
// DATA patches (a vtable slot, or one of the engine's cached pointer tables)
// do not hold here: the engine bypasses the vtable (per-object copies,
// per-object wrappers) and repairs its tables every few seconds, so such a
// hook only sees the ~32 post-process quads that pass through one patched
// call site, not the world (thousands of draws a frame) or the interface.
//
// The GPU only draws when d3d11.dll's own CContext::Draw* code runs.  Hooking
// that code in place (MinHook, a jmp at the function entry) is un-bypassable:
// whatever object, table or wrapper the engine calls through, it ends here
// with the real context as `this`.  Nothing the engine does to its tables can
// revert a patch inside d3d11.dll.
namespace UiHook
{
using LogFn = void (*)(const char*, ...);
// Which engine output (eye) the calling thread is building: 0 in mono and
// AER, 0/1 in native same-frame stereo.
using ReadOutputFn = int (*)();

// Must run BEFORE any vtable patch touches these contexts, so the slots still
// hold the real d3d11 implementations.  Idempotent.
bool Install(ID3D11Device* device, ID3D11DeviceContext* immediate,
    ID3D11DeviceContext* deferred, LogFn log, ReadOutputFn readOutput);

// Frame boundary and backbuffer identity.  Call from Present BEFORE the mod's
// own rendering.
void OnPresent(IDXGISwapChain* swapChain);

// Thread-local guard: draws issued while this is set are the mod's own
// (ImGui, capture) and are neither counted nor fitted.
void SetModRender(bool on);

// Native same-frame stereo is on: the interface may be drawn once per output.
void SetNativeStereo(bool active);
void SetWorldIcons(bool active);   // [AERICONS] stereo OR AER

void SetFitEnabled(bool enabled);
bool GetFitEnabled();
void SetFit(float scaleX, float scaleY, float offsetX, float offsetY);
unsigned long long GetFitDrawCount();

// [HUDLAYER] Redirect interface draws into a texture of the mod's own instead of
// the game frame, so the presenter can show ONE interface image to both eyes
// on its own composition layer.  Alternate-eye rendering shows each eye a
// different game frame; any interface element that changes then lands in one
// eye a frame before the other, which reads as flicker.  A shared layer
// cannot flicker, and its size and distance in the headset are free choices.
void SetHudLayerEnabled(bool enabled);
bool GetHudLayerEnabled();
// The presenter says whether a headset is actually consuming the layer; the
// interface stays in the game frame while nothing is.
void SetHudLayerConsumer(bool active);
// [TEXBIAS] mip LOD bias added to world-texture samplers, -2 .. 0 (0 = untouched).
void SetTextureBias(float bias);
float GetTextureBias();
// Once per present: this frame's interface texture (owned here, game device)
// and whether any interface draw landed in it.
ID3D11Texture2D* TakeHudFrame(bool* hasContent);
// [MENUHUD] render target of the interface layer while a headset consumes it (else null).
ID3D11RenderTargetView* HudOverlayTarget();
ID3D11Texture2D* HudOverlayTexture();
void MarkHudContent();
// [UIWORLD] this frame's world-anchored interface draws (markers), or null.
ID3D11Texture2D* TakeHudWorldFrame(bool* hasContent);
// Off = markers stay on the panel with everything else.
void SetHudWorldRouting(bool enabled);
ID3D11Texture2D* TakeObjectiveFrame(ObjectiveWorld::Frame* frame,unsigned slot=0);
ID3D11Texture2D* TakeInteractionFrame(ObjectiveWorld::Frame* frame);
ID3D11Texture2D* TakeTargetFrame(ObjectiveWorld::Frame* frame);
// [UILENS] lens the interface projects with (P00, P11), measured; false = not yet seen.
bool GetUiLens(float* p00, float* p11);
// [UICAM] base and head-applied camera column 2 (row-vector view matrix), from the head writer.
void SetCameraAxes(const float* baseCol2, const float* headCol2);
unsigned long long GetHudRedirectCount();
}
