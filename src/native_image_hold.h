#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>

// Diagnostic only: keep the last released stereo images AND all projection
// metadata together. The caller must skip RenderEyes while Reuse returns true.
// xrEndFrame references the most recently released image of each swapchain.
class NativeImageHold
{
public:
    using Views = std::array<XrCompositionLayerProjectionView, 2>;
    void Reset() { valid_ = false; views_ = {}; space_ = XR_NULL_HANDLE; epoch_ = 0; }
    bool Reuse(bool requested, bool native, XrSpace space, std::uint64_t epoch)
    {
        if (!requested || !native || !space || space != space_ || epoch != epoch_)
            Reset();
        return valid_;
    }
    // Only call after a fresh pair was released and xrEndFrame succeeded.
    void Capture(const Views& views, XrSpace space, std::uint64_t epoch)
    {
        views_ = views; space_ = space; epoch_ = epoch; valid_ = true;
    }
    const Views& Projection() const { return views_; }
private:
    Views views_{};
    XrSpace space_{};
    std::uint64_t epoch_{};
    bool valid_{};
};