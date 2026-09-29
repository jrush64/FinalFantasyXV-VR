#pragma once
// [GBUF] AFW milestone 1: find and capture FFXV's scene DEPTH and VELOCITY
// (motion-vector) buffers.
//
// Why: AFW renders one eye per frame and reconstructs the other by warping
// the previous frame's image of that eye with per-pixel motion vectors and
// depth.  Both buffers exist in this engine (DeferredManager creates
// "currVelocity/prevVelocity" and "depthStencil"), but the exe sets no D3D
// debug names, so they have to be recognised by how they are used.
//
// What it does, read-only apart from copies into its own textures:
//   1. Census: every render target and depth-stencil the engine binds
//      (MinHook on OMSetRenderTargets, the route that works here), with
//      format, size, MRT slot, how many draws land while it is bound, and the
//      engine-announced pass those draws belong to.  Reported as a table.
//   2. Candidates: the depth-stencil with the most draws and the two-channel
//      full-size render target with the most draws.  Locked after a settling
//      period and named in the log.
//   3. Capture: the moment a candidate is UNBOUND after real draws landed in
//      it this frame (the G-buffer pass ending), CopyResource it into a twin.
//      Mid-scene, on the same context, so a later clear cannot trample it.
//   4. Dump: three consecutive frames of depth twin + velocity twin +
//      backbuffer, subsampled, to <dll dir>\gbufdump\ for offline viewing
//      with tools\gbuf_to_png.py.  A warp is validated on captured frame
//      pairs before it ever reaches a headset.
// Gameplay defaults off; research builds default on.
// ini: [Probe] GBuf=1 explicitly enables the census (required by experimental AFW).
#include <d3d11.h>

struct IDXGISwapChain;

namespace GBufferCensus
{
// Engine pass announcement (already extracted to a safe local string).
void OnPass(const char* name);
// From the OMSetRenderTargets detours, BEFORE the original call.
void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv);
// From the ClearDepthStencilView detour.
void OnClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv);
// From BeginDraw for every non-mod draw.
void OnDraw(ID3D11DeviceContext* context, unsigned vertices = 0, unsigned start = 0, int base = 0, unsigned lane = 0);
// Once per present, on the present thread: reports, captured-per-present
// lines, and (AER only, hard-capped) dumps with a camera sidecar.
void OnPresent(IDXGISwapChain* swapChain, bool aerEnabled);
// [AFW] The depth twin captured for the frame that just presented (the one
// OnPresent accounted for), plus the projection+view the engine uploaded for
// that frame's G-buffer pass (from the velocity twin, whose last unbind is
// the deferred pass; the depth twin's last unbind is a late pass under a
// different projection).  False when this frame produced no capture.
bool GetPresentedFrame(ID3D11Texture2D** depthTwin, float* cam32, bool* camValid);
// [EYEFULL] One-shot: capture and dump EVERY full-size colour target of the
// next frame, so the engine's full-resolution per-eye images can be identified
// by eye rather than inferred.  Native stereo renders each eye at the whole
// backbuffer size and then scales it into its half, so those images already
// exist; submitting them directly would be 4x the pixels per eye for free.
// Costs one frame of copies, then disarms itself.
void ArmColorDump();
bool CaptureEnabled();
void VertexMapped(ID3D11Resource* resource,UINT subresource,D3D11_MAP type,void* bytes);
void VertexUnmapping(ID3D11Resource* resource,UINT subresource);
void VertexUpdated(ID3D11Resource* resource,UINT subresource,const D3D11_BOX* box,const void* data);
void VertexCopying(ID3D11Resource* destination,ID3D11Resource* source,UINT destinationOffset,const D3D11_BOX* box);
void RememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count);
void RememberShader(IUnknown* shader, const void* bytes, size_t length);
}
