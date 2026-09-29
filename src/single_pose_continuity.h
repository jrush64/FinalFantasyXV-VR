#pragma once
#include <openxr/openxr.h>
#include <cstdint>

// A short association gap may reuse only images captured with verified poses.
// Limit holds so a persistent loss cannot freeze gameplay indefinitely.
class SinglePoseContinuity {
public:
    void Begin(int mode, XrSpace space, std::uint64_t epoch) {
        if (mode != mode_ || space != space_ || epoch != epoch_ || !space) Reset();
        mode_=mode; space_=space; epoch_=epoch;
    }
    void Reset() { verified_=0; held_=0; }
    bool Retain(bool exact, bool pairReady) {
        if (exact) { held_=0; return false; }
        if (!space_ || verified_!=3 || !pairReady || held_>=2) return false;
        ++held_; return true;
    }
    void Copied(unsigned eyes, bool exact) {
        verified_ = exact ? (verified_|eyes) : (verified_&~eyes);
        if (exact) held_=0;
    }
private:
    unsigned verified_{}, held_{};
    int mode_{-1};
    XrSpace space_{};
    std::uint64_t epoch_{};
};
