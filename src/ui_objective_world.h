#pragma once
#include <cmath>
#include <cstdint>
namespace ObjectiveWorld {
constexpr unsigned TextureSize=512;
constexpr unsigned MaxMarkers=4;
constexpr float TexelsPerCanvasPixel=2.0f;
// A standard 40-canvas-pixel objective ring is a one-metre billboard.
constexpr float MetersPerCanvasPixel=0.025f;
constexpr float QuadMeters=TextureSize/TexelsPerCanvasPixel*MetersPerCanvasPixel;
// A one-metre ring, with a readable 1.5-degree minimum and a 1.75-degree near cap.
// Scale the whole artwork once, including its ring, symbol, arrow and stem.
inline float ReadableQuadMeters(float forwardMeters) {
    const float scale=std::fmin(std::fmax(1.f,forwardMeters*0.02618144f),forwardMeters*0.03054564f);
    return QuadMeters*scale;
}
// Keep the compositor quad inside the range already used by the visible marker
// layer. This preserves the world-derived ray and monocular angular size, but
// intentionally compresses stereo depth for objectives farther than 20 metres.
// Use radial distance so a head turn does not change the depth scale by itself.
constexpr float MaxCompositionDistance=20.f;
inline float CompositionScale(const float* view) {
    const float distance=std::hypot(view[0],view[1],view[2]);
    return distance>MaxCompositionDistance ? MaxCompositionDistance/distance : 1.f;
}
struct Anchor { std::uint64_t id{},time{}; float point[3]{},screen[2]{}; };
struct Frame {
    Anchor anchor{}; unsigned draws{},firstOrdinal{};
    float texelsPerCanvasPixel=TexelsPerCanvasPixel;
    float bodyShift{},captureOffset[2]{},canvasDiameter=40;
};
inline float CaptureScale(float anchorY,float centreY,float height) {
    // The stem is first. Its top plus 64px covers the ring/symbol above it.
    const float extent=std::fmax(64.f,std::fabs(centreY-anchorY)+height*.5f)+64.f;
    return std::fmin(TexelsPerCanvasPixel,TextureSize*.45f/extent);
}
inline bool InGroup(const Frame& f,unsigned ordinal,float x,float y,float width,float height) {
    if(!f.draws || ordinal<f.firstOrdinal || ordinal-f.firstOrdinal>16 ||
       !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height))return false;
    return width>0 && width<=96 && height>0 && height<=1024 &&
        std::fabs(x-f.anchor.screen[0])<=64 &&
        std::fabs(y-f.anchor.screen[1])+height*.5f<=TextureSize*.49f/f.texelsPerCanvasPixel;
}
inline bool RootX(unsigned kind,float x,float scale,float u,float v,float& root) {
    root=x;
    if(kind==3) return std::isfinite(root);
    if(kind!=2) return false;
    const float tx=u*320,ty=v*112;
    if(std::fabs(tx-1)<.75f && std::fabs(ty-56)<.75f) root+=20*scale;
    else if(std::fabs(tx-85)<.75f && std::fabs(ty-56)<.75f) root+=21*scale;
    else if(!((std::fabs(tx-43)<.75f && std::fabs(ty-56)<.75f) ||
              (std::fabs(tx-85)<.75f && std::fabs(ty-79)<.75f) ||
              (std::fabs(tx-1)<.75f && std::fabs(ty-98)<.75f))) return false;
    return std::isfinite(root);
}
struct History {
    Anchor points[128]{}; unsigned writes{};
    void Add(const Anchor& a) { points[writes++%128]=a; }
    bool Find(float rootX,float canvasY,std::uint64_t now,Anchor& out,float yTolerance=128) const {
        bool found=false; const unsigned n=writes<128?writes:128;
        for(unsigned age=0;age<n;++age) {
            const auto& a=points[(writes-1-age)%128];
            if(!a.id || now<a.time || now-a.time>250 ||
               std::fabs(a.screen[0]-rootX)>.01f || std::fabs(a.screen[1]-canvasY)>yTolerance) continue;
            if(found && a.id!=out.id) return false; // overlapping candidates are not an identity
            if(!found) {out=a;found=true;}
        }
        return found;
    }
};
inline bool ViewPoint(const float* world,const float* view,float* result) {
    if(!world || !view) return false;
    for(unsigned col=0;col<3;++col) {
        result[col]=world[0]*view[col]+world[1]*view[4+col]+world[2]*view[8+col]+view[12+col];
        if(!std::isfinite(result[col])) return false;
    }
    // Render-bank view is right-handed: visible points have negative Z.
    return result[2]<-.1f && result[2]>-100000;
}
}
