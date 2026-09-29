#pragma once
// [AFW] Alternate Frame Warping, milestone 2: synthesize the eye the engine did
// NOT render this frame from the eye it did, at the same instant.
//
// Geometry (measured): both AER
// eyes share the projection, and the other eye's world-to-view matrix differs
// only by a translation along view x.  So a source pixel at depth d lands in
// the other eye at the same row, shifted by
//     disparity_px = deltaT * P00 * (W/2) / (-z),   -z = -P32 / (d + P22)
// Three compute passes on the game's immediate context:
//   1. splat: every source pixel writes its depth (InterlockedMin, nearest
//      wins) into the destination column it lands on (two columns, to close
//      cracks from magnification);
//   2. gather: every destination pixel with a splatted depth reprojects back
//      and bilinearly samples the source colour; no depth = hole;
//   3. fill: holes take the nearer-in-row valid neighbour on the FARTHER
//      (background) side, the classic disocclusion rule.
// Same instant means characters and foliage need no motion vectors here; the
// temporal (previous other-eye frame) fill is a later step.
#include <d3d11.h>

namespace AfwSynth
{
struct Params
{
    float p00{}, p22{}, p32{}; // row-vector projection terms of the rendered eye
    float deltaT{};            // t_other.x - t_rendered.x (view-space metres)
};
// Returns the module-owned RGBA16F texture holding the synthesized eye, or
// nullptr (no shaders, mismatched sizes).  Valid until the next call.
ID3D11Texture2D* Synthesize(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* color, ID3D11Texture2D* depthTwin, const Params& params);
void Shutdown();
}
