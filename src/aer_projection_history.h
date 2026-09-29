#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>

// Metadata belongs to the last released image, not the current frame's lens.
// Callers must invalidate after a failed transfer that touched a swapchain.
class AerProjectionHistory
{
public:
    using View = XrCompositionLayerProjectionView;
    using Views = std::array<View, 2>;
    void Reset() { valid_ = {}; views_ = {}; }
    void Begin(int mode, XrSpace space, std::uint64_t epoch)
    {
        if (mode != mode_ || space != space_ || epoch != epoch_ || !space)
            Reset();
        mode_ = mode; space_ = space; epoch_ = epoch;
    }
    void Store(std::size_t eye, const View& view)
    {
        if (eye >= 2 || !space_ || !view.subImage.swapchain) return;
        views_[eye] = view;
        valid_[eye] = true;
    }
    bool Load(std::size_t eye, View& view) const
    {
        if (eye >= 2 || !valid_[eye]) return false;
        view = views_[eye];
        return true;
    }
    bool Pair(Views& views) const
    {
        if (!valid_[0] || !valid_[1]) return false;
        views = views_;
        return true;
    }
private:
    Views views_{};
    std::array<bool, 2> valid_{};
    XrSpace space_{};
    std::uint64_t epoch_{};
    int mode_{-1};
};
