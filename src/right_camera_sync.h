#pragma once
#include <cmath>
#include <cstring>
// Matrices here are row-vector world-to-view. Keep the original projection.
inline bool IsSyncCameraView(const float* m)
{
    for (int i=0;i<16;++i) if (!std::isfinite(m[i])) return false;
    if (std::fabs(m[15]-1.0f)>.001f || std::fabs(m[3])>.001f ||
        std::fabs(m[7])>.001f || std::fabs(m[11])>.001f) return false;
    for (int c=0;c<3;++c)
    {
        float length=0; for(int r=0;r<3;++r) length+=m[r*4+c]*m[r*4+c];
        if(length<.9f || length>1.1f) return false;
    }
    return true;
}
inline bool SelectMatchingLeftRenderCamera(const float* source32,
    const float* head16, const float* bank16, const float* render16, float* local32)
{
    if (!IsSyncCameraView(source32) || !IsSyncCameraView(bank16) ||
        std::memcmp(source32,head16,64)!=0 || std::memcmp(bank16,render16,64)!=0)
        return false;
    std::memcpy(local32,source32,128);
    std::memcpy(local32,bank16,64);
    return true;
}
