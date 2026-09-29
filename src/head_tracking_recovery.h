#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HeadTrackingRecovery
{
// Caller serializes access. This class never modifies the engine camera.
struct Health
{
    std::uintptr_t committedSource{}, candidate{};
    float committed[16]{};
    std::uint64_t committedAt{}, observedAt{}, mismatchSince{};
    unsigned misses{};
    bool valid{};

    static bool Usable(const float* matrix)
    {
        if (!matrix) return false;
        for (unsigned i=0;i<16;++i)
            if (!std::isfinite(matrix[i])) return false;
        return std::fabs(matrix[15]-1.0f)<0.01f;
    }
    void Reset() { *this = {}; }
    void Commit(std::uintptr_t source, const float* matrix, std::uint64_t now)
    {
        if (!source || !Usable(matrix)) return;
        committedSource=source;
        std::copy(matrix,matrix+16,committed);
        committedAt=now;
        valid=true;
    }
    void Observe(std::uintptr_t source, const float* matrix, std::uint64_t now, bool poseAvailable)
    {
        if (!valid || !poseAvailable || !source || !Usable(matrix))
        {
            misses=0; mismatchSince=0; observedAt=0; candidate=0;
            return;
        }
        bool matches=source==committedSource && now>=committedAt && now-committedAt<=1000;
        for (unsigned i=0;i<16 && matches;++i)
            matches=std::fabs(matrix[i]-committed[i])<=0.00001f;
        observedAt=now;
        if (matches)
        {
            candidate=source; misses=0; mismatchSince=0;
            return;
        }
        if (!misses || candidate!=source)
        {
            candidate=source; mismatchSince=now; misses=1;
        }
        else if (misses<1000000) ++misses;
    }
    bool NeedsRecovery(std::uint64_t now) const
    {
        return valid && candidate && misses>=12 && now>=mismatchSince &&
            now-mismatchSince>=300 && now>=observedAt && now-observedAt<=500;
    }
};
}
