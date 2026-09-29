#pragma once
// [PROFILE] Sampling profiler for the game's present thread, native stereo only.
//
// [FRAMETIME] In native stereo the present thread spends
// 12-20 ms per frame between the mod's Present hook exit and its next entry,
// against under 1 ms in mono; the mod's own cost is 0.5 ms.  That is the
// engine's stereo frame and it caps the regular cadence at half of a 120 Hz
// panel.  This samples where that thread sits (leaf RIP by module, nearest
// system-DLL export, innermost game-code return addresses) about once a
// millisecond and logs the top sites every 3000 samples, for a bounded
// number of reports.  Diagnostic build only; no game state is touched.
#include <windows.h>

namespace PresentProfiler
{
void Start();                       // spawns the sampler thread (idempotent)
void NotePresentThread(DWORD id);   // called from the Present hook
}
