#pragma once
#include <d3d11.h>
#include <openxr/openxr.h>
#include <cstdint>
namespace NativeDrawPose {
struct Pose {
    XrPosef eyes[2]{};
    std::uint64_t serial{}, epoch{};
    float view[16]{};
    int renderedEye{-1}; // -1: native pair; 0/1: single-output camera
};
using Matcher = bool (*)(const float*, Pose&);
void Initialize(Matcher matcher, unsigned width, unsigned height, bool singleOutput = false);
void SetHooksReady(bool ready);
void SetActive(bool active);
void RememberVertexShader(ID3D11VertexShader*, const void*, SIZE_T);
void Mapped(ID3D11DeviceContext*,ID3D11Resource*,D3D11_MAP,void*);
void Unmapping(ID3D11DeviceContext*,ID3D11Resource*);
void Invalidate(ID3D11Resource*);
void Updated(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,bool version1);
void Copied(ID3D11Resource*);
void BeforeDraw(ID3D11DeviceContext*);
void Finished(ID3D11DeviceContext*,ID3D11CommandList*);
void BeginExecute();
bool InsideExecute();
void Executed(ID3D11CommandList*);
Pose CaptureBoundary();
Pose Pending();
bool ReadStamp(ID3D11DeviceContext*,ID3D11Buffer*,Pose&);
// [GBUF] The raw projection (16 floats) and view (16 floats) the engine last
// uploaded to the camera buffer bound on this context's VS slot 0/1.  Stamped
// on every whole-buffer UpdateSubresource regardless of the pose route's
// active state, so it also works in AER.
bool ReadRawCamera(ID3D11DeviceContext*, float* projectionThenView32);
}
