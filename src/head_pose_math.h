#pragma once

#include <openxr/openxr.h>
#include <cmath>

namespace HeadPoseMath
{
// Upright orientation from the same unfiltered yaw/pitch used by the camera.
// Removing quaternion Z alone would also change the viewing direction.
inline XrQuaternionf YawPitch(float yaw, float pitch)
{
    const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
    const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
    return {cy*sp, sy*cp, -sy*sp, cy*cp};
}

// Same yaw/pitch extraction as PublishHeadPose. The native camera never applies
// roll, including when its submitted pose comes from the current locate/fallback.
// Preserve the eye position and sample time; this is not a temporal correction.
inline XrPosef UprightPose(XrPosef eyePose, const XrQuaternionf& head)
{
    const float yaw = std::atan2(2.0f * (head.w * head.y + head.x * head.z),
        1.0f - 2.0f * (head.y * head.y + head.x * head.x));
    const float sinPitch = 2.0f * (head.w * head.x - head.z * head.y);
    const float pitch = std::asin(sinPitch < -1.0f ? -1.0f :
        (sinPitch > 1.0f ? 1.0f : sinPitch));
    eyePose.orientation = YawPitch(yaw, pitch);
    return eyePose;
}

// The game applies (rawYaw-centerYaw, rawPitch-centerPitch) to its base camera.
// Its tag must express that SAME rotation in the tracking reference space.
// Subtracting Euler angles does not equal removing a pitched center quaternion.
inline XrQuaternionf CenteredCameraOrientation(float rawYaw, float rawPitch,
    float centerYaw, float centerPitch)
{
    const auto a=YawPitch(centerYaw,centerPitch);
    const auto b=YawPitch(rawYaw-centerYaw,rawPitch-centerPitch);
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
}
