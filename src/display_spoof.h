#pragma once
// [DISPQ] Render-above-display for native stereo.
//
// Native stereo submits HALF of the desktop backbuffer per eye, letterboxed
// to the game's 16:9 lens, so a 2560x1440 backbuffer gives each eye a
// 1280x720 picture ([SPLITFOV] rect=1280x720+0+360).  AER
// hands each eye the whole 2560x1440 and is visibly sharper.  The engine
// sizes its backbuffer from the display it runs on, so this module makes
// every display-size query the game can ask (user32, gdi32, DXGI output)
// agree that the game's display is [Display] BackbufferWH pixels, and adds
// that mode to the enumerated mode lists.
//
// Keyed on the SIZE of the display the game uses (SourceWH, auto = largest
// attached display), not on "primary": the primary can be a
// virtual (streaming) monitor while the game runs on another
// display.
//
// Off unless the ini names a target larger than the source.  Every hook
// is a pure pass-through when inactive.
#include <windows.h>

struct IUnknown;

namespace DisplaySpoof
{
// DllMain attach.  Reads ffxv-vr.ini next to `self`, snapshots the real
// display sizes, installs the user32/gdi32 hooks.  Logs are buffered until
// FlushEarlyLog() because the log file does not exist yet.
void InstallEarly(HMODULE self);
// From the DXGI factory hook: patches IDXGIOutput's vtable once so mode
// lists and desktop rects tell the same story as user32.
void HookDxgiOutputs(IUnknown* factory);
// After OpenLog(): writes the buffered attach-time lines.
void FlushEarlyLog();
bool Active();
void TargetSize(unsigned* width, unsigned* height);
// Swapchain creation observer (which size did the game finally ask for?).
void NoteSwapchain(const char* api, unsigned width, unsigned height, unsigned format,
    int windowed, HWND window);
}
