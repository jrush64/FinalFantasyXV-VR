#pragma once
// [WORLDLOCK] Read-only measurement of frame-to-frame jitter.
// Rotation matrices are 3x3 row-major, column-vector convention.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WorldLock
{
// World-to-view rotation of an FFXV camera (row-vector world-to-view matrix,
// axes in columns: right = m0,m4,m8; up = m1,m5,m9; back = m2,m6,m10).
inline void ViewRotation(const float* m, float* a)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            a[c * 3 + r] = m[r * 4 + c];
}
inline void QuatRotation(float x, float y, float z, float w, float* r)
{
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}
inline void Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}
inline void MulT(const float* a, const float* b, float* out)   // a * b^T
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3] * b[c * 3] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2];
}
// |A-B| of two rotations = 2*sqrt(2)*sin(angle/2): precise near zero.
inline float AngleDeg(const float* a, const float* b)
{
    double sum = 0.0;
    for (int k = 0; k < 9; ++k) { const double d = static_cast<double>(a[k]) - b[k]; sum += d * d; }
    const double s = std::min(1.0, std::sqrt(sum) / 2.8284271247461903);
    return static_cast<float>(2.0 * std::asin(s) * 57.29577951308232);
}

// Per-sample: step = rotation since last sample, jitter = change of that
// step (the second difference: a steady turn is 0, a shake is not).
struct Series
{
    float prev[9]{}, prevStep[9]{};
    int have{};
    std::uint64_t n{}, moving{};
    double step{}, jitter2{}, jitterMax{}, jitterMoving2{};
    void Reset() { *this = {}; }
    // Returns the jitter of this sample (deg), or -1 when not yet defined.
    float Add(const float* r, bool headMoving)
    {
        float out = -1.0f;
        if (have >= 1)
        {
            float d[9]{};
            MulT(r, prev, d);
            const float s = AngleDeg(r, prev);
            step += s;
            if (have >= 2)
            {
                const float j = AngleDeg(d, prevStep);
                ++n;
                jitter2 += static_cast<double>(j) * j;
                if (j > jitterMax) jitterMax = j;
                if (headMoving) { ++moving; jitterMoving2 += static_cast<double>(j) * j; }
                out = j;
            }
            std::copy(d, d + 9, prevStep);
        }
        std::copy(r, r + 9, prev);
        have = std::min(have + 1, 2);
        return out;
    }
    double Rms() const { return n ? std::sqrt(jitter2 / static_cast<double>(n)) : 0.0; }
    double RmsMoving() const { return moving ? std::sqrt(jitterMoving2 / static_cast<double>(moving)) : 0.0; }
    double MeanStep() const { return n ? step / static_cast<double>(n) : 0.0; }
};
}
