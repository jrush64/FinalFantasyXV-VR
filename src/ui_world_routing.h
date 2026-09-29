#pragma once
#include <cmath>
#include <cstdint>

namespace UiWorldRouting
{
// UI sprite VS 0x3076710, verified from the retail executable. Used only for
// trace coordinates; the original shader still performs the real draw.
// cb must contain at least 136 floats. Floats 16..31 are deliberately unused.
inline bool ProjectSprite(const float* cb, float localX, float localY, float& x, float& y)
{
    float v[4]{};
    for (unsigned i = 0; i < 4; ++i)
        v[i] = localX * cb[i] + localY * cb[4+i] + cb[8+i] + cb[12+i];
    for (unsigned i = 0; i < 2; ++i)
        v[i] = cb[134] > 0 ? v[i] - .001f : std::floor(v[i] + .4999f);
    float clip[4]{};
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            clip[col] += v[row] * cb[32 + row*4 + col];
    if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[3]) || std::fabs(clip[3]) < 1e-6f)
        return false;
    x = clip[0] / clip[3]; y = clip[1] / clip[3];
    return std::isfinite(x) && std::isfinite(y);
}

// Match an external symbol only to a same-pass objective ring. Coordinates
// are in the game's 1920x1080 canvas, reconstructed through the real sprite VS.
// No prior-frame anchors, camera-block guesses, or texture-wide icon routing.
struct SpriteBounds { float x{}, y{}, width{}, height{}; };
struct ObjectiveGroups
{
    struct Ring { SpriteBounds bounds; std::uint32_t ordinal; } rings[16]{};
    unsigned count{};
    void Clear() { count = 0; }
    void Add(const SpriteBounds& b, std::uint32_t ordinal)
    {
        if (b.width < 4 || b.height < 4 || b.width > 96 || b.height > 96) return;
        if (count < 16) rings[count++] = {b, ordinal};
    }
    bool Matches(const SpriteBounds& b, std::uint32_t ordinal,bool huntBadge=false) const
    {
        if (b.width < 4 || b.height < 4 || b.width > 80 || b.height > 80) return false;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& r = rings[i];
            if (ordinal < r.ordinal || ordinal - r.ordinal > 64) continue;
            // Ring and imported icon have the same centre. Four canvas pixels
            // allow their distinct pixel snapping, not arbitrary nearby HUD.
            if (std::fabs(b.x-r.bounds.x) <= 4 && std::fabs(b.y-r.bounds.y) <= 4 &&
                b.width <= r.bounds.width + 8 && b.height <= r.bounds.height + (huntBadge?12:8))
                return true;
        }
        return false;
    }
};

// Distance shadow/face glyphs follow the attached shared symbol
// consecutively, 25..46 canvas pixels above its centre. Do not route a font
// atlas globally or inherit this association across a pass/ordinal gap.
struct ObjectiveLabelRun {
    SpriteBounds symbol{}; unsigned next{},used{};
    void Clear(){*this={};}
    void Start(const SpriteBounds& b,unsigned ordinal){symbol=b;next=ordinal+1;used=0;}
    bool Matches(const SpriteBounds& b,unsigned ordinal) const {
        if(!next || ordinal!=next || used>=32 || !std::isfinite(b.x) || !std::isfinite(b.y))return false;
        const float scale=symbol.width/24.f;
        if(scale<.5f || scale>2.f)return false;
        return b.width>0 && b.width<=48*scale && b.height>0 && b.height<=36*scale &&
            std::fabs(b.x-symbol.x)+b.width*.5f<=96*scale &&
            b.y-b.height*.5f>=symbol.y-64*scale && b.y+b.height*.5f<=symbol.y-18*scale;
    }
    void Advance(){++next;++used;}
};

// The limit applies to the contents of one layer, not to discarded draws.
// Native stereo clears/rebuilds the layer for its second interface pass.
struct PassBudget
{
    static constexpr std::uint32_t Limit = 48;
    std::uint32_t used{};
    void ClearLayer() { used = 0; }
    bool Take()
    {
        if (used >= Limit) return false;
        ++used;
        return true;
    }
};
}
