#pragma once
#include <cmath>
#include <cstdint>
inline bool ShouldPinFovTransitionStart(float duration, std::uint32_t mode, float start)
{
    // Native mode6 and duration0 bypass the explicit-start transition path.
    return std::isfinite(duration) && duration > 0.0f && mode != 6 &&
        (start == -1.0f || (std::isfinite(start) && start > .05f && start < 3.1f));
}
