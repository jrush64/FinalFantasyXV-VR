#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "right_camera_sync.h"

namespace HeadPoseAssociation
{
// Caller holds the pose-state lock. The copied pose is keyed by the full camera
// matrix actually committed, not by how many frames ago it was sampled.
template<class Pose, std::size_t Capacity = 64>
class History
{
public:
    void Clear() { samples_ = {}; cameras_ = {}; writes_ = 0; }
    void Publish(const Pose& pose) { samples_[pose.serial % Capacity] = pose; }
    bool Sample(std::uint64_t serial, Pose& result) const
    {
        const auto& sample = samples_[serial % Capacity];
        if (!serial || sample.serial != serial) return false;
        result = sample;
        return true;
    }
    bool Commit(std::uint64_t serial, const float* view, std::uint64_t now,
        const Pose* committedPose = nullptr)
    {
        const auto& pose = samples_[serial % Capacity];
        if (!serial || pose.serial != serial || !view || !IsSyncCameraView(view)) return false;
        if (committedPose && committedPose->serial != serial) return false;
        auto& slot = cameras_[writes_++ % Capacity];
        // Camera-specific metadata (including AER eye) belongs to this commit,
        // not the head sample: both eyes may deliberately reuse one sample.
        slot.pose = committedPose ? *committedPose : pose;
        slot.time = now;
        std::memcpy(slot.view, view, sizeof(slot.view));
        return true;
    }
    bool Find(const float* view, std::uint64_t now, Pose& result) const
    {
        if (!view || !IsSyncCameraView(view)) return false;
        const auto count = writes_ < Capacity ? writes_ : Capacity;
        for (std::uint64_t age = 0; age < count; ++age)
        {
            const auto& slot = cameras_[(writes_ - 1 - age) % Capacity];
            if (!slot.pose.serial || now < slot.time || now - slot.time > 1000) continue;
            // These are copies of the same matrix. Tolerances could associate
            // nearly matching animation cameras with the wrong headset pose.
            if (std::memcmp(slot.view, view, sizeof(slot.view)) == 0)
            {
                result = slot.pose;
                return true;
            }
        }
        return false;
    }
    // Committed cameras newest first (age 0 = newest).
    bool Recent(std::uint64_t age, std::uint64_t now, Pose& pose, float* view) const
    {
        const auto count = writes_ < Capacity ? writes_ : Capacity;
        if (age >= count) return false;
        const auto& slot = cameras_[(writes_ - 1 - age) % Capacity];
        if (!slot.pose.serial || now < slot.time || now - slot.time > 1000) return false;
        pose = slot.pose;
        std::memcpy(view, slot.view, sizeof(slot.view));
        return true;
    }
private:
    struct Camera { Pose pose{}; float view[16]{}; std::uint64_t time{}; };
    std::array<Pose, Capacity> samples_{};
    std::array<Camera, Capacity> cameras_{};
    std::uint64_t writes_{};
};

// Upload order follows the game's graphics command stream. Preserve invalid
// entries too: removing one would shift every following pose onto another frame.
// Caller holds the pose-state lock for all operations.
template<class Pose, std::size_t Capacity = 64>
class OrderedFrames
{
public:
    struct Frame { Pose pose{}; std::uint64_t epoch{}, time{}, sequence{}; };
    void Clear() { first_ = count_ = 0; desynced_ = false; }
    bool Push(const Pose& pose, std::uint64_t epoch, std::uint64_t now)
    {
        if (desynced_) return false;
        if (count_ == Capacity)
        {
            first_ = count_ = 0;
            desynced_ = true;
            return false;
        }
        frames_[(first_ + count_) % Capacity] = {pose, epoch, now, ++sequence_};
        ++count_;
        return true;
    }
    bool Pop(std::uint64_t epoch, std::uint64_t now, Frame& result)
    {
        if (desynced_) { Clear(); return false; }
        if (!count_) return false;
        const auto frame = frames_[first_];
        first_ = (first_ + 1) % Capacity;
        --count_;
        if (frame.epoch != epoch || now < frame.time || now - frame.time > 1000)
        { Clear(); return false; }
        result = frame;
        return true;
    }
    std::size_t Size() const { return count_; }
private:
    std::array<Frame, Capacity> frames_{};
    std::size_t first_{}, count_{};
    std::uint64_t sequence_{};
    bool desynced_{};
};

// Present-thread storage freezes the frame popped from the shared queue before
// Tick/xrWaitFrame. A later upload cannot replace the selected pose mid-submit.
template<class Pose>
class FrameLatch
{
public:
    bool Active() const { return depth_ != 0; }
    void Upload(const Pose& pose, std::uint64_t epoch)
    { pending_ = pose; pendingEpoch_ = epoch; }
    void Begin()
    {
        if (depth_++ != 0) return;
        frozen_ = pending_; frozenEpoch_ = pendingEpoch_; pending_ = {};
    }
    void End()
    {
        if (depth_ && --depth_ == 0) frozen_ = {};
    }
    bool Snapshot(std::uint64_t epoch, Pose& output) const
    {
        if (!depth_ || !frozen_.serial || frozenEpoch_ != epoch) return false;
        output = frozen_; return true;
    }
private:
    Pose pending_{}, frozen_{};
    std::uint64_t pendingEpoch_{}, frozenEpoch_{};
    unsigned depth_{};
};
}
