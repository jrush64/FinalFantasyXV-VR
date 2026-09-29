#pragma once

// Expensive research capture is opt-in at build time. Functional camera
// discovery, recovery, stereo, projection and HUD work do not use this gate.
#ifndef FFXV_RESEARCH_DIAGNOSTICS
#define FFXV_RESEARCH_DIAGNOSTICS 0
#endif
#ifndef FFXV_MARKER_DIAGNOSTICS
#define FFXV_MARKER_DIAGNOSTICS 0
#endif
namespace ResearchDiagnostics
{
inline constexpr bool MarkerCapture = FFXV_MARKER_DIAGNOSTICS != 0;
inline constexpr bool Enabled = FFXV_RESEARCH_DIAGNOSTICS != 0;
}
