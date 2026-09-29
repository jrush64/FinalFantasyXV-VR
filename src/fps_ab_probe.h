#pragma once
// [FPSAB] Frame-cost A/B between the three presentation modes (native
// same-frame stereo / AER / mono), plus a GPU-bound discriminator.
//
// Question it answers: at the SAME backbuffer, does dropping to one engine
// view per frame (AER, mono) buy frame time over native stereo, and is the
// frame limited by the CPU or the GPU?  This decides whether an AFW-style
// warp (one eye rendered, the other reconstructed) can pay for itself.
//
// Per present it records, keyed by the mode the frame was presented in:
//   interval = hook entry -> next hook entry (1/fps)
//   work     = interval minus xrWaitFrame and DXGI Present (engine + mod)
//   wait     = xrWaitFrame + DXGI Present (pacing / back-pressure)
//   gpuLag   = presents elapsed before the GPU finished the frame's work
//              (a TIMESTAMP query ended just before the real Present, polled
//              at later presents; 1 = GPU keeps up, >=2 sustained = GPU-bound)
// Reported per mode every 600 presents and on every mode change; running
// totals (frames, wall time, fps, megapixels shaded per frame) are appended
// so a 20-second stand in each mode gives a direct comparison.  Flat scenes
// (loading, menus, movies) are skipped.  Diagnostic only; nothing in the
// game is touched.  ini: [Probe] FpsAb=0 disables it.
#include <windows.h>

struct IDXGISwapChain;

namespace FpsAbProbe
{
// Right before the real DXGI Present: ends this frame's GPU timestamp query.
void BeforePresent(IDXGISwapChain* swapChain);
// Right after the real DXGI Present: records the frame and polls old queries.
// monoFallback = native stereo is on but the engine rendered ONE output this
// frame (AUTOMONO).  Those frames are counted as "native1" so they cannot
// inflate the native average: they are the engine's own one-view speed, which
// is exactly the number an AFW-style one-view mode would be compared against.
void AfterPresent(bool nativeStereo, bool aer, bool sceneFlat, bool monoFallback,
    long long entryQpc, long long beforePresentQpc, long long afterPresentQpc,
    long long xrWaitMicros);
}
