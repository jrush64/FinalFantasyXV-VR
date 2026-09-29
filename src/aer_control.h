#pragma once

namespace AerControl
{
bool IsAerEnabled();
void SetAerEnabled(bool enabled);
float GetHalfEyeMeters();
void SetHalfEyeMeters(float meters);
bool GetSwapEyes();
void SetSwapEyes(bool swap);
// [AFW] AER only: synthesize the eye the engine did not render this frame
// from the one it did (same instant, depth-based), so both eyes are current
// every frame.  Numpad 1 toggles it.
bool GetAfwEnabled();
void SetAfwEnabled(bool enabled);
// [AFWFLIP] Which side the synthesized eye sits on.  The magnitude is measured
// (2 x HalfEyeMeters) but the mapping from the engine's eye label to the
// headset's left/right swapchain depends on SwapEyes and on the engine's own
// convention, so it is a toggle: one position is right and the other is
// visibly wrong (depth inverted / doubled).  Numpad 2.
bool GetAfwFlip();
void SetAfwFlip(bool flip);
// [SPLIT] Native same-frame stereo (F9): the engine renders both eyes
// every frame.  Eye separation is the engine's own baseline (centimetres;
// stock 6.0); 0 means "leave the stock value".
bool IsNativeStereoEnabled();
void SetNativeStereoEnabled(bool enabled);
float GetNativeBaselineCm();
void SetNativeBaselineCm(float centimetres);
// Native stereo has its OWN swap and convergence; nothing is shared with AER.
bool GetNativeSwapEyes();
void SetNativeSwapEyes(bool swap);
float GetNativeConvergence();
void SetNativeConvergence(float tangentShift);
bool GetSplitHonestLens();
void SetSplitHonestLens(bool enabled);
// Both eyes move: left = centre - b/2 (gameplay camera shifted), right =
// centre + b/2.  Off = the original rig (left = plain camera, right = camera + b).
bool GetNativeSymmetric();
void SetNativeSymmetric(bool enabled);
float GetNativeFill();
void SetNativeFill(float fill);
// [AUTOMONO] Interactive conversations freeze in the engine's two-output mode;
// while an interaction action runs, drop to one output (mono, head-tracked).
// 0 = keep stereo, 1 = mono (one output, tracked), 2 = switch to AER.
int GetConversationMode();
void SetConversationMode(int mode);
int GetConversationActive();
float GetConvergence();
void SetConvergence(float tangentShift);
float GetFillFactor();
void SetFillFactor(float fill);
bool GetLiveFovOverrideEnabled();
void SetLiveFovOverrideEnabled(bool enabled);
float GetLiveFovDegrees();
void SetLiveFovDegrees(float degrees);
int GetFovParamIndex();
void SetFovParamIndex(int index);
int GetFovParamValue();
int GetFovSinkOffset();
int GetFovSinkKind();
void SetFovSink(int offset, int kind);
bool GetLensFeedEnabled();
void SetLensFeedEnabled(bool enabled);
bool GetEyeHoldEnabled();
bool GetAerSimSync();              // [AERSYNC] both eyes of a pair render one game moment
void SetAerSimSync(bool enabled);
void SetEyeHoldEnabled(bool enabled);
bool GetAerFreshHeadPose();          // [AERFRESH]
void SetAerFreshHeadPose(bool enabled);
bool GetFovAllModes();
void SetFovAllModes(bool enabled);
float GetNativeHeadTiming();
void SetNativeHeadTiming(float offset);
// [LOOKAHEAD] frames past this present's display time to predict the head
// pose for: <0 auto (measured pipeline depth), 0 old behaviour, else fixed.
float GetHeadLookaheadFrames();
void SetHeadLookaheadFrames(float frames);
int GetHeadPipelineDepth();
// [TAGMODE] native submit pose: current (located for this present) vs rendered (FIFO).
bool GetNativeTagCurrent();
void SetNativeTagCurrent(bool current);
bool GetSixDofEnabled();
void SetSixDofEnabled(bool enabled);
float GetSixDofPositionScale();
void SetSixDofPositionScale(float scale);
bool GetCustomFirstPersonEnabled();
void SetCustomFirstPersonEnabled(bool enabled);
void GetCustomFirstPersonOffsets(float* right, float* up, float* forward);
void SetCustomFirstPersonOffsets(float right, float up, float forward);
int GetRenderEye();
// [UIFIT] Pull the interface into the readable part of the headset by
// shrinking the raster viewport for interface draws only.
bool GetUiFitEnabled();
void SetUiFitEnabled(bool enabled);
void GetUiFit(float* scaleX, float* scaleY, float* offsetX, float* offsetY);
void SetUiFit(float scaleX, float scaleY, float offsetX, float offsetY);
unsigned long long GetUiFitDrawCount();
// [HUDLAYER] The interface on its own head-locked layer, one image to both
// eyes: no alternate-eye flicker, and free choice of size and distance.
bool GetHudLayerEnabled();
void SetHudLayerEnabled(bool enabled);
void GetHudLayerParams(float* distanceMeters, float* widthMeters, float* verticalOffsetMeters);
void SetHudLayerParams(float distanceMeters, float widthMeters, float verticalOffsetMeters);
bool GetHudLensExact();
void SetHudLensExact(bool enabled);
void SetHudWorldDistance(float meters);
float GetHudWorldDistance();
void SetHudWorldBaseCamera(bool enabled);
bool GetHudWorldBaseCamera();
unsigned long long GetHudLayerDrawCount();
// [MONO2D] One image to both eyes on loading screens, menus and movies.
bool GetFlatSceneMonoEnabled();
void SetFlatSceneMonoEnabled(bool enabled);
bool GetSceneIsFlat();
void Recenter();
// [KEYBIND] Rebindable control keys (Insert menu > Tracking >
// Controls). VR-enable is polled in main.cpp's Tick(), so it is exposed here
// like every other cross-module setting.
// [VRMODESAVE] 1 = Stereo, 2 = AER: what the head tracking key starts from Mono.
int GetPreferredVrMode();          // 1 Stereo, 2 AER, 3 Mono
bool IsMonoEnabled();              // [MONOVR] head-tracked Mono
void SetMonoEnabled(bool enabled);
void SetPreferredVrMode(int mode);
int GetMonoKey();                    // [MONOKEY]
void SetMonoKey(int vk);
bool GetDecoupledPitch();            // [DECOUPLEPITCH]
void SetDecoupledPitch(bool enabled);
int GetVrEnableKey();
void SetVrEnableKey(int vk);
// [DEVKEYS] Every research/debug hotkey (F2/F3/F4/F6/F7/F8/F10/F11,
// Numpad 0-3, the flash-probe's Numpad 7/8/9, this mod's own frame-grabber
// Numpad 9) is gated on this.  Off by default on every fresh install; ini
// [Debug] DeveloperKeys=1 turns them back on without a rebuild.
bool GetDeveloperKeysEnabled();
// [LENSWATCH] log the rendered lens + FOV-setter state at the next frame.
void RequestLensSnapshot();
}
