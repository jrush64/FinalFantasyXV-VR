#pragma once
#include <d3d11.h>
#include <cstdint>
#include <openxr/openxr.h>

namespace GpuPoseTrace
{
struct Match { std::uint64_t serial{}, epoch{}; XrQuaternionf orientation{}; };
using Matcher = bool (*)(const float* view, Match& result);
void SetEnabled(bool enabled);
void Initialize(ID3D11Device* device, ID3D11DeviceContext* immediate, Matcher matcher);
void SetNativeActive(bool active);
// Called immediately before an original game draw. Reads actual bound VS CBs.
void BeforeDraw(ID3D11DeviceContext* context);
void Finished(ID3D11DeviceContext* context, ID3D11CommandList* list);
void Executed(ID3D11CommandList* list);
// Boundary immediately before the game backbuffer is copied to XR's shared image.
std::uint64_t CaptureBoundary();
void PoseAssigned(std::uint64_t capture, const XrPosef& pose,
    std::uint64_t associatedSerial, std::uint64_t latestSerial, std::uint64_t epoch,
    std::uint64_t directSerial=0);
void Poll(); // immediate context only, one nonblocking pass; no Flush or waits
#ifdef GPU_POSE_TRACE_TEST
struct TestResult { unsigned samples{}, ready{}, matched{}, executed{}, replayed{}, readbacks{}, empty{}, bytes{}, copied{}, first{}, slot{}; std::uint64_t serial{}, capture{}; float firstValue{}; HRESULT query{E_PENDING},map{E_PENDING}; BOOL complete{}; unsigned captured{}; };
TestResult Inspect();
#endif
}
