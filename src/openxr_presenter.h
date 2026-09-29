#pragma once

#include <cstdint>
#include "ui_objective_world.h"

struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace RetailXr
{
void OnPresent(IDXGISwapChain* swapChain, bool nativeStereoEnabled, bool aerEnabled);
// AER renders one full backbuffer eye per game frame. The presenter owns the
// eye cadence so the camera writer and the captured swapchain image agree.
int GetAerRenderEye();
// [AERSEQ] The camera build owns the eye cadence.  It derives its eye from its
// own sequence parity (L,R,L,R by construction) and publishes {seq, eye} into
// a ring; the presenter consumes oldest-first, one per present, so the eye
// label always belongs to the pixels actually on screen.  Peek before applying
// the offset, publish once the camera write has landed, so a build that fails
// never claims a frame.
int PeekAerBuildEye();
// [AERTAG] headSerial = the head sample the camera write applied (for the
// second eye of a held pair, the FIRST eye's sample); committedView = the
// world-to-view matrix actually written, used as AER's world-icon camera.
void PublishAerBuildEye(std::uint64_t headSerial = 0, const float* committedView = nullptr);
bool AerStampsArmed();
// [MONO2D] Loading screens, menus and pre-rendered movies carry no 3D camera.
// Alternating eyes across them builds no depth and puts a shimmer on a flat
// image, so the presenter shows one frame to both eyes for the duration.
void SetSceneIsFlat(bool flat);
bool GetSceneIsFlat();
void SetFlatSceneMonoEnabled(bool enabled);
bool GetFlatSceneMonoEnabled();
void ResetAerEyeSequence();
void SetAerConvergence(float tangentShift);
float GetAerConvergence();
// Live game projection (row-vector P00/P11 focals plus the right-eye skew
// P20).  When set, submitted layers claim the FOV the game actually
// rendered instead of a scaled runtime FOV; unset values fall back to the
// old presentation math.
void SetGameProjection(float focalX, float focalY, float skew,
    float referenceFocalX, float referenceFocalY);
// Headset coverage: 1.0 = geometry-honest (black border), higher = fuller
// with mild magnification.  Cycled at runtime with F7.
void SetFillFactor(float fill);
float GetFillFactor();
// [SPLIT] Native same-frame stereo presentation controls.  Swap exchanges
// the two halves; the honest lens claims the game's real per-eye projection
// (as AER does) instead of the scaled runtime FOV.
void SetSplitSwapEyes(bool swap);
bool GetSplitSwapEyes();
void SetSplitConvergence(float tangentShift);
float GetSplitConvergence();
void SetSplitHonestLens(bool enabled);
bool GetSplitHonestLens();
// Native stereo's own headset fill (1.0 = the exact rendered lens).
void SetSplitFill(float fill);
float GetSplitFill();
// [AUTOMONO] Native stereo is on but the engine is rendering ONE output this
// frame: present the full mono frame to both eyes, tracking kept.
void SetSplitMonoFallback(bool active);
// [VIDEOFIT] presents since the engine last built a stereo eye pair (0 = 3D this frame).
void SetFlatPresents(std::uint32_t presents);
// [VIDEOFIT] width in degrees of the flat screen used for videos/loading screens (30-110).
void SetVideoScreenFov(float degrees);
// [SHARPEN] contrast-adaptive sharpening of both eye images, 0 = off .. 1.
void SetSharpen(float strength);
// [MENUQUAD] the Insert menu texture (game device) and the menu window rectangle in it.
void SetMenuLayer(ID3D11Texture2D* texture, bool visible, int x, int y, int w, int h);
bool IsMenuLayerAvailable();
// [MENUQUAD] width of the headset menu panel in metres at 1.2 m (0.3-1.6).
void SetMenuWidth(float metres);
float GetMenuWidth();
// [MENUDRAG] where the panel sits, in metres, added to its fixed 1.2 m
// forward position. (0,0) = centred, exactly as before.
void SetMenuOffset(float x, float y);
// [DOCK] videos / loading screens stay where you were facing instead of
// following the head.
void SetDockFlatScreens(bool enabled);
// [MONOVR] head-tracked Mono: the AER pipeline with no eye offset, every
// frame captured into BOTH eyes (tagged with its exact render pose).
void SetMonoSubmit(bool enabled);
bool GetMonoSubmit();
bool GetDockFlatScreens();
void GetMenuOffset(float* x, float* y);
// [VRHINT] tick count (GetTickCount64) the XR device first came up, 0 = not yet.
unsigned long long GetXrReadyTick();
float GetSharpen();
float GetVideoScreenFov();
// Latest centered LOCAL-space HMD pose. Position is expressed in meters in
// camera-local right/up/forward axes; orientation is yaw/pitch/roll radians.
// The render hook marks the exact sample it used so submission can carry
// the matching pose instead of applying current-pose timewarp to old pixels.
bool GetHeadPose(float* yaw, float* pitch, float* roll, float* right,
    float* up, float* forward, std::uint64_t* serial);
void MarkHeadPoseRendered(std::uint64_t serial);
// Native stereo: associate the committed matrix with its raw headset sample,
// then latch the render-bank match on the render thread at DXGI Present entry.
void RecordHeadCameraPose(std::uint64_t serial, const float* committedView, int renderedEye = -1);
void RecordNativeRenderCamera(const float* bankView, bool singleOutput = false);
void BeginHeadPosePresent();
void EndHeadPosePresent();
// Optional native orientation timing calibration; zero retains matched timing.
float GetNativeHeadTiming();
void SetNativeHeadTiming(float offset);
// [LOOKAHEAD] Native stereo pose prediction target, in frames past this
// present's display time: <0 = auto (the measured pipeline depth), 0 = the
// old behaviour (predict for this present), otherwise fixed.
float GetHeadLookaheadFrames();
void SetHeadLookaheadFrames(float frames);
int GetHeadPipelineDepth();
// [TAGMODE] true = submit with the pose located for this present; false = exact FIFO match.
bool GetNativeTagCurrent();
void SetNativeTagCurrent(bool current);
// [FRAMETIME] microseconds the last OnPresent spent blocked in xrWaitFrame.
long long GetLastXrWaitMicros();
void ResetHeadPoseCenter();
// Temporary native diagnostic, never persisted: freeze both released images
// and their projection metadata. Runtime tracking continues. Pause toggles it.
void SetNativeImageHold(bool enabled);
bool GetNativeImageHold();
// When enabled, retained AER views keep the same positional sample that the
// game camera rendered instead of stripping center-head translation.
void SetSixDofCameraEnabled(bool enabled);
// [AFW] presenter-side switch (mirrors AerControl::SetAfwEnabled).
void SetAfwEnabled(bool enabled);
bool GetAfwEnabled();
void SetAfwFlip(bool flip);
bool GetAfwFlip();
// [HUDLAYER] The interface, redirected by the draw hook into a texture on the
// game device, is shown on its own head-locked quad layer that both eyes
// share.  Set once per present BEFORE OnPresent.
void SetHudLayer(ID3D11Texture2D* gameTexture, bool hasContent);
void SetHudWorldLayer(ID3D11Texture2D* gameTexture, bool hasContent);
void SetObjectiveLayer(ID3D11Texture2D* gameTexture,const ObjectiveWorld::Frame& frame,unsigned slot=0);
void SetInteractionLayer(ID3D11Texture2D* gameTexture,const ObjectiveWorld::Frame& frame);
void SetTargetLayer(ID3D11Texture2D* gameTexture,const ObjectiveWorld::Frame& frame);
bool IsHudLayerConsuming();
void SetHudLayerEnabled(bool enabled);
bool GetHudLayerEnabled();
void SetHudLayerParams(float distanceMeters, float widthMeters, float verticalOffsetMeters);
void SetHudLensExact(bool enabled);
void SetHudWorldDistance(float meters);
void SetUiLens(float p00, float p11);
void SetHudWorldBaseCamera(bool enabled);
bool GetHudWorldBaseCamera();
float GetHudWorldDistance();
bool GetHudLensExact();
void GetHudLayerParams(float* distanceMeters, float* widthMeters, float* verticalOffsetMeters);
void Shutdown();
}
