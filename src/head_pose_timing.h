#pragma once
#include "head_pose_math.h"
#include "head_pose_association.h"
#include <algorithm>
#include <limits>

namespace HeadPoseTiming
{
// Calibration relative to the matched camera, not an assumed absolute pipeline
// depth. Positive uses an older orientation; negative uses a newer orientation.
// This changes only native submission orientation. Camera motion, eye positions,
// queue order and the matched serial remain untouched. Zero is bit-identical.
inline bool Angles(const XrQuaternionf& q, float& yaw, float& pitch)
{
    const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!std::isfinite(norm) || norm < 1e-8f) return false;
    const float inverse = 1.0f / std::sqrt(norm);
    const float x=q.x*inverse, y=q.y*inverse, z=q.z*inverse, w=q.w*inverse;
    yaw=std::atan2(2.0f*(w*y+x*z), 1.0f-2.0f*(y*y+x*x));
    pitch=std::asin(std::clamp(2.0f*(w*x-z*y),-1.0f,1.0f));
    return std::isfinite(yaw) && std::isfinite(pitch);
}
template<class Pose, std::size_t Capacity>
bool AdjustOrientation(const HeadPoseAssociation::History<Pose, Capacity>& history,
    const Pose& matched, float offset, Pose& result)
{
    result=matched;
    if (!matched.serial || !std::isfinite(offset) || offset==0.0f ||
        offset < -1.0f || offset > 1.0f) return false;
    const bool older=offset>0.0f;
    if ((older && matched.serial<=1) ||
        (!older && matched.serial==std::numeric_limits<std::uint64_t>::max())) return false;
    Pose adjacent{};
    if (!history.Sample(older ? matched.serial-1 : matched.serial+1, adjacent)) return false;
    Pose adjusted=matched;
    const float amount=std::fabs(offset);
    constexpr float pi=3.14159265358979323846f;
    for (int eye=0;eye<2;++eye)
    {
        float aYaw{},aPitch{},bYaw{},bPitch{};
        if (!Angles(matched.poses[eye].orientation,aYaw,aPitch) ||
            !Angles(adjacent.poses[eye].orientation,bYaw,bPitch)) return false;
        if (amount==1.0f)
            adjusted.poses[eye].orientation=adjacent.poses[eye].orientation;
        else
        {
            const float delta=std::remainder(bYaw-aYaw,2.0f*pi);
            // Interpolate yaw/pitch and reconstruct upright, so a diagonal turn
            // cannot introduce roll through quaternion blending.
            adjusted.poses[eye].orientation=HeadPoseMath::YawPitch(
                aYaw+delta*amount,aPitch+(bPitch-aPitch)*amount);
        }
    }
    result=adjusted;
    return true;
}
}
