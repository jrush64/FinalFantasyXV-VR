#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include "runtime_log.h"
#include "research_diagnostics.h"
#include "perf_census.h"
namespace FullEye {
inline bool initialized{},enabled{},active{},ready{},invalid{};
inline unsigned count{},frames{};
inline ID3D11Texture2D* back{};
inline ID3D11Texture2D* images[2]{};
inline ID3D11RenderTargetView* outputs[2]{};
inline ID3D11Resource* identity{};
inline ID3D11PixelShader* shader{};
inline D3D11_TEXTURE2D_DESC desc{};
// [FULLRES] The engine's per-eye picture is as large as its RENDER resolution,
// which is the backbuffer only at 100 %.  Any picture of the backbuffer's shape
// that is larger than the half-size packed eye is captured, 1:1 up to the
// backbuffer size, into the top-left usedW x usedH of each eye image.  The
// picture's format is free: the game's own shader converts it on the way out.
struct Plan { bool ok{}; unsigned w{}, h{}; };
inline Plan PlanFor(unsigned backW,unsigned backH,unsigned srcW,unsigned srcH){
 Plan p{};if(!backW||!backH||!srcW||!srcH)return p;
 const double a=double(srcW)*backH,b=double(srcH)*backW;
 if(a>b*1.01||b>a*1.01)return p;            // not the eye's shape
 if(2ull*srcW<=backW+8ull)return p;          // no larger than the packed half: nothing to gain
 p.ok=true;p.w=srcW<backW?srcW:backW;p.h=srcH<backH?srcH:backH;return p;
}
inline unsigned usedW{},usedH{},readyW{},readyH{};
inline unsigned rejectW{},rejectH{},rejectFormat{},logged{};
inline unsigned long long lastLogKey{};
// [VIDEOFIT] Where the game drew into the backbuffer this frame: 1 = a picture
// wider than 60 % of it (a full-screen video/loading screen), 2 = a picture in
// the left half (a per-eye stereo composite).  Largest-area viewport wins.
// Published at the frame boundary as lastLayout for the presenter.
struct Layout { int kind{}; float x{}, y{}, w{}, h{}; };
inline Layout frameLayout{}, lastLayout{};
inline void Observe(const D3D11_VIEWPORT& vp){
 const float W=float(desc.Width),area=vp.Width*vp.Height;
 if(!(W>0.f)||vp.Width<=0.f||vp.Height<=0.f)return;
 const auto bigger=[&](int kind){return frameLayout.kind!=kind || area>frameLayout.w*frameLayout.h;};
 if(vp.Width>0.6f*W){ if(bigger(1)) frameLayout={1,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height}; }
 else if(frameLayout.kind!=1 && vp.Width>=0.25f*W && vp.TopLeftX+vp.Width<=0.5f*W+2.f){ if(bigger(2)) frameLayout={2,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height}; }
}
template<class T> inline void Release(T*& p){if(p)p->Release();p=nullptr;}
inline void Init(){if(initialized)return;initialized=true;wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto* slash=wcsrchr(path,L'\\');if(!slash)return;wcscpy_s(slash+1,MAX_PATH-size_t(slash+1-path),L"ffxv-vr.ini");enabled=GetPrivateProfileIntW(L"Display",L"FullEye",1,path)!=0;}
inline void Reset(){Release(back);for(auto*& p:outputs)Release(p);for(auto*& p:images)Release(p);Release(identity);Release(shader);desc={};count=0;ready=false;active=false;invalid=false;usedW=usedH=readyW=readyH=0;}
inline void Boundary(ID3D11Texture2D* buffer,bool stereo){
 Init();ready=enabled&&active&&stereo&&count==2&&!invalid&&images[0]&&images[1]&&usedW&&usedH;
 readyW=ready?usedW:0;readyH=ready?usedH:0;
 if(enabled&&active&&stereo){
  const unsigned long long key=(ready?1ull:0ull)|(unsigned long long)(count>3?3:count)<<1|(invalid?8ull:0ull)|(unsigned long long)readyW<<4|(unsigned long long)rejectW<<24|(unsigned long long)rejectFormat<<44;
  if(key!=lastLogKey&&logged<60){lastLogKey=key;++logged;char line[260];
   sprintf_s(line,"[FULLRES] per-eye picture %s: eye image %ux%u of backbuffer %ux%u fmt=%u (captures=%u conflict=%u; last picture passed over %ux%u fmt=%u)",
    ready?"FULL SIZE":"packed half",readyW,readyH,desc.Width,desc.Height,unsigned(desc.Format),count,invalid?1u:0u,rejectW,rejectH,rejectFormat);RetailLogLine(line);}
 }
 if(back!=buffer){Reset();if(buffer){back=buffer;back->AddRef();back->GetDesc(&desc);}}
 if(ResearchDiagnostics::Enabled && enabled && (++frames<=4 || frames%300==0)){char line[180];sprintf_s(line,"[FULLEYE] complete=%u copies=%u invalid=%u size=%ux%u",ready,count,invalid,desc.Width,desc.Height);RetailLogLine(line);}
 count=0;invalid=false;usedW=usedH=0;rejectW=rejectH=rejectFormat=0;Release(identity);Release(shader);active=enabled&&stereo&&back;
 lastLayout=frameLayout;frameLayout={};
}
inline void OnPresent(IDXGISwapChain* sc,bool stereo){ID3D11Texture2D* b{};if(sc)sc->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&b));Boundary(b,stereo);Release(b);}
inline bool CopyPair(ID3D11DeviceContext* c,ID3D11Texture2D* first,ID3D11Texture2D* second,bool swap){
 if(!ready||!c||!first||!second)return false;
 D3D11_TEXTURE2D_DESC a{},b{};first->GetDesc(&a);second->GetDesc(&b);
 if(a.Width!=desc.Width||a.Height!=desc.Height||a.Format!=desc.Format||b.Width!=desc.Width||b.Height!=desc.Height||b.Format!=desc.Format)return false;
 c->CopyResource(first,images[swap?1:0]);c->CopyResource(second,images[swap?0:1]);return true;
}
template<class Draw> inline void Replay(ID3D11DeviceContext* c,unsigned vertices,Draw draw){
 if(!active||!c||vertices<3||vertices>6||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 if(PerfCensus::bypassFullEye.load(std::memory_order_relaxed))return; // [PERF] Numpad8: eyes fall back to the half-size rect
 struct Timer{std::uint64_t t0=PerfCensus::Tsc();~Timer(){PerfCensus::Add(PerfCensus::FullEyeTicks,PerfCensus::Tsc()-t0);}} timer;
 ID3D11RenderTargetView* rt{};ID3D11DepthStencilView* depth{};c->OMGetRenderTargets(1,&rt,&depth);
 ID3D11Resource* target{};if(rt)rt->GetResource(&target);bool eligible=target==back&&!depth;Release(rt);Release(depth);Release(target);if(!eligible)return;
 {UINT n=1;D3D11_VIEWPORT vp{};c->RSGetViewports(&n,&vp);if(n==1)Observe(vp);} // [VIDEOFIT]
 if(vertices!=6)return;
 ID3D11ShaderResourceView* srv{};c->PSGetShaderResources(0,1,&srv);if(!srv)return;
 D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);ID3D11Resource* resource{};srv->GetResource(&resource);Release(srv);
 ID3D11Texture2D* source{};resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&source));Release(resource);if(!source)return;
 D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
 const Plan plan=PlanFor(desc.Width,desc.Height,d.Width,d.Height);
 eligible=source!=back&&plan.ok&&d.SampleDesc.Count==1&&d.ArraySize==1&&d.MipLevels==1&&sd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&sd.Texture2D.MostDetailedMip==0;
 if(!eligible){if(source!=back){rejectW=d.Width;rejectH=d.Height;rejectFormat=unsigned(d.Format);}Release(source);return;}
 ID3D11PixelShader* ps{};c->PSGetShader(&ps,nullptr,nullptr);
 if(count==0){identity=source;identity->AddRef();shader=ps;if(shader)shader->AddRef();}
 if(count>=2||identity!=source||shader!=ps||!ps){invalid=true;++count;Release(ps);Release(source);return;}
 Release(ps);
 if(!images[count]){ID3D11Device* dev{};c->GetDevice(&dev);d.Width=desc.Width;d.Height=desc.Height;d.Format=desc.Format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET;d.CPUAccessFlags=0;d.MiscFlags=0;if(FAILED(dev->CreateTexture2D(&d,nullptr,&images[count])) || FAILED(dev->CreateRenderTargetView(images[count],nullptr,&outputs[count])))invalid=true;dev->Release();}

 if(images[count] && outputs[count]){
    UINT nvp=1;D3D11_VIEWPORT viewport{};c->RSGetViewports(&nvp,&viewport);
    if(nvp!=1){invalid=true;++count;Release(source);return;}
    ID3D11RenderTargetView* saved[8]{};ID3D11DepthStencilView* savedDepth{};c->OMGetRenderTargets(8,saved,&savedDepth);
    UINT nr=16;D3D11_RECT rects[16]{};c->RSGetScissorRects(&nr,rects);
    // Native packing occupies W/2 x H/2, centred vertically in the backbuffer.
    // Enlarge the raster viewport around that eye's rectangle; keep the game's
    // shaders, textures, constants, blend and colour transform exactly intact.
    // [FULLRES] scale = captured picture / packed half (2 at 100 % render size).
    const float scale=2.f*float(plan.w)/float(desc.Width);
    auto full=viewport;full.TopLeftX=scale*(viewport.TopLeftX-(count?desc.Width*.5f:0));
    full.TopLeftY=scale*(viewport.TopLeftY-desc.Height*.25f);full.Width*=scale;full.Height*=scale;
    D3D11_RECT fullRect{0,0,LONG(plan.w),LONG(plan.h)};
    usedW=plan.w;usedH=plan.h;
    const float black[4]{};c->ClearRenderTargetView(outputs[count],black);
    c->OMSetRenderTargets(1,&outputs[count],nullptr);c->RSSetViewports(1,&full);c->RSSetScissorRects(1,&fullRect);
    draw();PerfCensus::Add(PerfCensus::FullEyeReplays);
    c->OMSetRenderTargets(8,saved,savedDepth);c->RSSetViewports(1,&viewport);c->RSSetScissorRects(nr,rects);
    for(auto*& view:saved)Release(view);Release(savedDepth);
 } else invalid=true;
 ++count;Release(source);
}
}
