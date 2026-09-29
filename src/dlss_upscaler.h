#pragma once
// [DLSS] DLSS 4 (NGX Super Resolution, DLAA mode) in place of the engine's own
// temporal resolve.  Addresses and layouts: docs/FFXV_ENGINE_REFERENCE.md
//
// What the D1 probe established and this module relies on:
//   - the resolve is ONE pixel shader in AfterPostEffect: colour = PS SRV 0
//     (RG11B10F), depth = SRV 3 (R32F_X8X24), velocity = SRV 6 (RG16F),
//     parameters = PS CB 0 (256 B), output = RTV 0 (RG11B10F)
//   - the parameter buffer is the engine's cbTemporalAA: c0 screen size,
//     c2.xy UV jitter, c3..c6 motion matrix (current UV+depth -> previous)
//   - velocity is total screen motion in UV, y-down, prevUV = uv + v, with a
//     (0, 1.0) sentinel on pixels that wrote none (static world, sky)
//
// Per frame, at the resolve draw (immediate context only):
//   1. a compute pass turns velocity into DLSS pixel motion vectors, rebuilding
//      the sentinel pixels from depth through the engine's motion matrix, and
//      copies depth into a plain R32F texture
//   2. NGX evaluates DLSS into the module's own UAV texture
//   3. that result is copied into the engine's resolve target and the
//      engine's own draw is skipped
// One NGX feature (one temporal history) per depth buffer, i.e. per eye family,
// so eyes never share a history.
//
// The SECOND eye in native stereo gets no engine resolve at all (retail strips
// output 1's post-effect banks), so there is no draw to take over.  Both eyes
// render in turn through ONE shared scene-colour target, and each eye runs the
// same post chain; the second eye's chain simply lacks the resolve.  So the
// draw that follows the resolve in the first eye is learnt, and in the second
// eye DLSS runs just before that same draw, in place on the scene colour,
// with that eye's own velocity and depth and the frame's jitter/motion matrix.
//
// Any failure turns the module off and the engine's TAA simply keeps running.
//
// Needs nvngx_dlss.dll (310.x) beside the game exe.  Always on whenever the
// game's anti-aliasing is TAA; other AA modes leave it idle.
#include <d3d11.h>

struct IDXGISwapChain;

namespace DlssUpscaler
{
// AER's engine output owns its native TAA path. Keep this module independent
// of AER code so the standalone smoke harness can link it directly.
void SetAerBypass(bool enabled);
// From BeginDraw for non-mod draws of <= 6 vertices/indices, with the caller's
// mod-render guard held (NGX and the module's compute pass issue their own calls).
// True = DLSS wrote the resolve output; the caller must skip the engine draw.
bool OnDraw(ID3D11DeviceContext* context);
// Parameter-buffer shadowing: the engine rewrites cbTemporalAA every frame,
// possibly in a pool of buffers; the last write before the draw is what the
// draw reads.
void OnMapped(ID3D11Resource* resource, D3D11_MAP type, void* data);
void OnUnmapping(ID3D11Resource* resource);
void OnUpdated(ID3D11Resource* resource, const D3D11_BOX* box, const void* data);
// From the OMSetRenderTargets detours (non-mod calls): learns which velocity
// buffer belongs to which depth buffer, and which eye's scene is being drawn.
void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv);
// From the Dispatch detours (non-mod): compute half of the left-eye empty-input fix.
void OnDispatch(ID3D11DeviceContext* context);
// [MVFIX] Every non-mod draw: captures the eye family's main-camera view constants
// on the first draw after its velocity pass was bound.
void OnGeometryDraw(ID3D11DeviceContext* context);
// [PAIRBURST] log per-frame eye-camera pairing detail for the next N presents.
void RequestPairBurst(unsigned presents);
// Once per present: hotkey, history bookkeeping, periodic status line.
void OnPresent();
}
