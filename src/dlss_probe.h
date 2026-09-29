#pragma once
// [DLSSP] Step 1 of the DLSS 4 / DLAA integration (docs/FFXV_ENGINE_REFERENCE.md).
//
// Read-only. Renders nothing, changes nothing, allocates no GPU resources.
// Question: which pass reads the engine's motion vectors, and what else does
// it read?  That pass is what a DLSS integration takes over.
//
// Why by identity: slot layouts and formats are not reliable here.  The
// engine binds all 16 slots on post draws (srvMask 0xFFFF, stale leftovers
// included), so format co-occurrence says nothing, and RG8 full-size targets
// outnumber any RG16F.  So the velocity texture is tracked by RESOURCE
// IDENTITY: every RG16F full-size render target the engine writes (the
// deferred pass's MRT velocity) and every full-size depth-stencil it writes
// are recorded as they are bound for output; then every full-screen draw AND
// every compute dispatch is checked for reading those exact resources.
//
// ini: [Probe] Dlss=0 disables.
#include <d3d11.h>

#include <cstdint>

struct IDXGISwapChain;

namespace DlssProbe
{
// From BeginDraw, for every non-mod draw.  `count` is the vertex or index
// count of the lane; `indexed` marks the DrawIndexed family.
void OnDraw(ID3D11DeviceContext* context, unsigned count, bool indexed);
// From the Dispatch / DispatchIndirect detours, for every non-mod dispatch.
void OnDispatch(ID3D11DeviceContext* context);
// From the OMSetRenderTargets detours, before the engine's call lands: learns
// which resources are the velocity and depth buffers.
void OnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* dsv);
// The engine's own pass announcement, already copied to a safe local string.
void OnPass(const char* name);
// From the Map / Unmap detours, so the candidate's parameter buffer can be
// read on the frame the engine writes it.
void OnMapped(ID3D11Resource* resource, D3D11_MAP type, void* data);
void OnUnmapping(ID3D11Resource* resource);
// Once per present, on the present thread.  The caller supplies the mode and
// the eye label for the frame about to be drawn, so this module links against
// nothing but D3D and the log (the smoke tests build it standalone).
void OnPresent(bool aerEnabled, bool nativeStereo, int renderEye);

// [GPUT] GPU frame timer (D3D11 timestamp queries on the immediate context).
// Frame fps is quantised by the display refresh, so it cannot show a GPU saving
// that stays inside one refresh slot; GPU milliseconds can.
//   span   = first engine command on the immediate context after a Present
//            -> the next Present, on the GPU timeline (includes any GPU wait
//            for CPU submissions inside the frame)
//   period = Present -> Present on the GPU timeline
// Each window is tagged with the current render width (the velocity target's
// width), so 100% and 50% resolution-scaling stretches separate themselves.
// Call after OnPresent, from the present hook, before the mod's own
// present-time rendering.
void OnPresentGpu(IDXGISwapChain* swapChain);
// From ExecuteCommandList (draws and dispatches call it internally): the first
// engine work on the immediate context after a Present stamps the frame start.
void OnImmediateWork(ID3D11DeviceContext* context);
// The pixel shader identified as the engine's temporal resolve (reads the
// velocity buffer AND scene depth), or 0 until it has been seen.
std::uintptr_t TemporalResolveShader();
// Identity of the game's immediate context (0 until the first present).
std::uintptr_t ImmediateContextKey();
}
