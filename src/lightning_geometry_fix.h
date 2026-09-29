#pragma once
#include <d3d11_1.h>
#include <atomic>
#include "runtime_log.h"
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstring>
#include <new>
#include <cstdio>
#include <algorithm>
#include <climits>
#include "research_diagnostics.h"
namespace LightningGeometryFix {
inline constexpr bool DiagnosticOutput=ResearchDiagnostics::Enabled;
inline const GUID kShaderTag={0x1175c042,0x3e88,0x42cb,{0x9a,0x23,0x75,0x19,0x33,0x62,0x99,0x81}};
inline const GUID kHashTag={0x1175c043,0x3e88,0x42cb,{0x9a,0x23,0x75,0x19,0x33,0x62,0x99,0x81}};
inline UINT ShaderTag(std::uint64_t h){return h==0x47E10DBD23AB11A1ull?1:h==0xFA4539217706DFF8ull?2:h==0x04EAEC979EDD5535ull?3:h==0x0B8CCCC7247B9460ull?4:0;}
inline void RememberShader(IUnknown* shader,const void* bytes,size_t length) {
    if(!shader || !bytes)return;
    std::uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<length;++i){hash^=static_cast<const unsigned char*>(bytes)[i];hash*=1099511628211ull;}
    UINT tag=ShaderTag(hash);
    ID3D11DeviceChild* child{};
    if(SUCCEEDED(shader->QueryInterface(__uuidof(ID3D11DeviceChild),reinterpret_cast<void**>(&child)))) {
        child->SetPrivateData(kHashTag,sizeof(hash),&hash); // [FLASHPROBE] names every shader
        if(tag)child->SetPrivateData(kShaderTag,sizeof(tag),&tag);
        child->Release();
    }
}
inline bool Tagged(ID3D11DeviceChild* shader,UINT expected) {
    UINT tag=0,size=sizeof(tag);
    return shader && SUCCEEDED(shader->GetPrivateData(kShaderTag,&size,&tag)) && tag==expected;
}

// CPU upload shadow, owned by the D3D resource. Never map/read back a GPU buffer.
inline const GUID kUploadTag={0x1ab24e79,0xc1a0,0x4012,{0x80,0x19,0x22,0x65,0x41,0x71,0xab,0x03}};
struct Upload final : IUnknown {
    std::atomic<ULONG> refs{1}; SRWLOCK lock=SRWLOCK_INIT;
    std::vector<unsigned char> bytes; UINT offset{}; bool known{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
};
// [TRACKSET] Every Map and UpdateSubresource used to call
// GetPrivateData to learn the buffer is not a lightning upload.  Tracked
// uploads are seeded in Reject only; remember those few pointers and answer
// "not tracked" without a COM call for everything else.
inline std::atomic<std::uintptr_t> g_tracked[16]{};
inline void Track(ID3D11Resource* r){
    const auto key=reinterpret_cast<std::uintptr_t>(r);
    for(auto& t:g_tracked)if(t.load(std::memory_order_relaxed)==key)return;
    for(auto& t:g_tracked){std::uintptr_t expected=0;if(t.compare_exchange_strong(expected,key))return;}
    g_tracked[key&15].store(key,std::memory_order_relaxed);
}
inline bool MaybeTracked(ID3D11Resource* r){
    const auto key=reinterpret_cast<std::uintptr_t>(r);
    for(auto& t:g_tracked)if(t.load(std::memory_order_relaxed)==key)return true;
    return false;
}
inline Upload* Get(ID3D11Resource* r){
    if(!r || !MaybeTracked(r))return nullptr;
    Upload* u{};UINT size=sizeof(u);
    r->GetPrivateData(kUploadTag,&size,&u);return u;
}
inline void Invalidate(ID3D11Resource* r){if(auto* u=Get(r)){
    AcquireSRWLockExclusive(&u->lock);u->known=false;ReleaseSRWLockExclusive(&u->lock);u->Release();}}
inline void Updated(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data){
    auto* u=Get(r);if(!u)return;
    AcquireSRWLockExclusive(&u->lock);u->known=false;
    // Deferred-context uploads cannot establish immediate-context ordering.
    if(c && c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE && !sub && data){
        ID3D11Buffer* b{};
        if(SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&b)))){
            D3D11_BUFFER_DESC d{};b->GetDesc(&d);b->Release();
            UINT first=box?box->left:0,last=box?box->right:d.ByteWidth;
            if(first<last && last<=d.ByteWidth && last-first<=8*1024*1024){
                try{u->bytes.resize(last-first);memcpy(u->bytes.data(),data,last-first);u->offset=first;u->known=true;}catch(...){u->known=false;}
            }
        }
    }
    ReleaseSRWLockExclusive(&u->lock);u->Release();
}


inline const GUID kLayoutTag={0x6229b731,0x1290,0x4891,{0x93,0x12,0x17,0x62,0x00,0x51,0x13,0x01}};
struct Layout {UINT positionFormat{},positionSlot{},positionOffset{UINT_MAX},colourFormat{},colourSlot{},colourOffset{UINT_MAX};};
inline void RememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* e,UINT n){
    if(!layout||!e)return;Layout v{};
    for(UINT i=0;i<n;++i){if(!e[i].SemanticName)continue;
        if(!_stricmp(e[i].SemanticName,"POSITION") && e[i].SemanticIndex==0){v.positionFormat=e[i].Format;v.positionSlot=e[i].InputSlot;v.positionOffset=e[i].AlignedByteOffset;}
        if(!_stricmp(e[i].SemanticName,"COLOR") && e[i].SemanticIndex==4){v.colourFormat=e[i].Format;v.colourSlot=e[i].InputSlot;v.colourOffset=e[i].AlignedByteOffset;}
    }layout->SetPrivateData(kLayoutTag,sizeof(v),&v);
}
inline std::atomic_bool g_sampleCounts[2][4097]{};
inline std::atomic<UINT> g_sampleFiles{};
inline std::atomic<UINT> g_sampleClasses[2]{};
inline void SaveFiniteSample(ID3D11DeviceContext* c,const unsigned char* data,UINT count,UINT offset,bool suspiciousColour){
    if constexpr (!DiagnosticOutput) return;
    const UINT category=suspiciousColour?1:0;
    if(count>4096 || g_sampleCounts[category][count].exchange(true))return;
    if(g_sampleClasses[category].fetch_add(1)>=8)return;
    const UINT index=g_sampleFiles.fetch_add(1);
    ID3D11InputLayout* input{};c->IAGetInputLayout(&input);Layout layout{};UINT bytes=sizeof(layout);
    const bool known=input && SUCCEEDED(input->GetPrivateData(kLayoutTag,&bytes,&layout));if(input)input->Release();
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto* tail=wcsrchr(path,L'\\');if(!tail)return;
    *(tail+1)=0;wcscat_s(path,L"lightning_cpu");CreateDirectoryW(path,nullptr);
    wchar_t file[MAX_PATH]{};swprintf_s(file,L"%s\\finite_%u_count%u.bin",path,index,count);
    HANDLE h=CreateFileW(file,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    bool saved=false;if(h!=INVALID_HANDLE_VALUE){DWORD written{};saved=WriteFile(h,data,count*36,&written,nullptr)&&written==count*36;CloseHandle(h);}
    char line[320];sprintf_s(line,"[LIGHTNING] CPU sample=%u count=%u offset=%u saved=%u layoutKnown=%u POSITION(format/slot/offset)=%u/%u/%u COLOR4=%u/%u/%u",index,count,offset,saved,known,layout.positionFormat,layout.positionSlot,layout.positionOffset,layout.colourFormat,layout.colourSlot,layout.colourOffset);RetailLogLine(line);
}
struct Validation {bool position{},colour{};float maxPosition{},maxColour{};};
inline Validation Validate(const unsigned char* bytes,UINT count){
    Validation result{};
    for(UINT i=0;i<count;++i){float v[7];memcpy(v,bytes+i*36,sizeof(v));
        for(UINT j=0;j<7;++j){const bool invalid=!std::isfinite(v[j])||std::abs(v[j])>1000000.0f;
            if(j<3){result.position|=invalid;if(DiagnosticOutput && std::isfinite(v[j]))result.maxPosition=(std::max)(result.maxPosition,std::abs(v[j]));}
            else{result.colour|=invalid || v[j]<-0.0001f || (j==6 && v[j]>1.0001f);if(DiagnosticOutput && std::isfinite(v[j]))result.maxColour=(std::max)(result.maxColour,std::abs(v[j]));}
        }
    }
    return result;
}
inline bool InvalidPositions(const unsigned char* b,UINT n){return Validate(b,n).position;}
inline std::atomic_bool g_rejected{};
inline std::atomic<unsigned long long> g_otherShaderRejected{};
inline std::atomic<unsigned long long> g_checked{},g_unknown{},g_rangeMiss{},g_badPosition{},g_badColour{},g_layoutMiss{};
inline std::atomic<unsigned> g_anomalies{};
inline std::atomic<ULONGLONG> g_nextReport{};
inline void ReportCoverage(bool known,bool inRange,UINT count,UINT start,UINT stride,UINT offset,Validation v){
    if constexpr (!DiagnosticOutput) return;
    const auto n=++g_checked;
    if(!known)++g_unknown;else if(!inRange)++g_rangeMiss;
    if(v.position)++g_badPosition;if(v.colour)++g_badColour;
    // Bounded CPU-only evidence. No image copies, file dumps, stack walks or GPU waits.
    if((v.maxPosition>10 || v.maxColour>100) && !v.position && !v.colour && g_anomalies.fetch_add(1)<16){
        char line[256];sprintf_s(line,"[LIGHTNING] finite anomaly count=%u start=%u stride=%u offset=%u maxPosition=%g maxColour=%g",count,start,stride,offset,v.maxPosition,v.maxColour);RetailLogLine(line);
    }
    const auto now=GetTickCount64();auto due=g_nextReport.load();
    if(now>=due && g_nextReport.compare_exchange_strong(due,now+10000)){
        char line[320];sprintf_s(line,"[LIGHTNING] coverage checked=%llu unknown=%llu rangeMiss=%llu badPosition=%llu badColour=%llu layoutMiss=%llu otherShaderRejected=%llu",n,g_unknown.load(),g_rangeMiss.load(),g_badPosition.load(),g_badColour.load(),g_layoutMiss.load(),g_otherShaderRejected.load());RetailLogLine(line);
    }
}
inline std::atomic_ullong g_rejectedDraws{};
inline bool Reject(ID3D11DeviceContext* c,UINT count,UINT start){
    if(!c || count<2 || count>4096 || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    D3D11_PRIMITIVE_TOPOLOGY top{};c->IAGetPrimitiveTopology(&top);
    if(top!=D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP && top!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)return false;
    ID3D11Buffer* b{};UINT stride{},off{};c->IAGetVertexBuffers(0,1,&b,&stride,&off);
    if(!b)return false;if(stride!=36){if constexpr(DiagnosticOutput)++g_layoutMiss;b->Release();return false;}
    ID3D11InputLayout* input{};c->IAGetInputLayout(&input);Layout layout{};UINT layoutBytes=sizeof(layout);
    const bool correct=input && SUCCEEDED(input->GetPrivateData(kLayoutTag,&layoutBytes,&layout)) && layout.positionFormat==DXGI_FORMAT_R32G32B32_FLOAT && layout.positionSlot==0 && layout.positionOffset==0 && layout.colourFormat==DXGI_FORMAT_R32G32B32A32_FLOAT && layout.colourSlot==0 && layout.colourOffset==12;
    if(input)input->Release();if(!correct){if constexpr(DiagnosticOutput)++g_layoutMiss;b->Release();return false;}
    if(start>(UINT_MAX-off)/36){b->Release();return false;}off+=start*36;
    auto* u=Get(b);
    if(!u){
        // Seed only confirmed lightning or corrupt Magitek-trail pairs. Once tracked,
        // validate other draws consuming the same buffer and typed vertex layout.
        ID3D11VertexShader* vs{};ID3D11PixelShader* ps{};
        c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);
        const bool seed=(Tagged(vs,1)&&Tagged(ps,2))||(Tagged(vs,3)&&Tagged(ps,4));if(vs)vs->Release();if(ps)ps->Release();
        if(!seed){b->Release();return false;}
        u=new(std::nothrow) Upload;if(u){b->SetPrivateDataInterface(kUploadTag,u);Track(b);}
    }
    bool reject=false,known=false,inRange=false;Validation validation{};
    if(u){AcquireSRWLockShared(&u->lock);
        known=u->known;
        inRange=known && off>=u->offset && off-u->offset<=u->bytes.size() && count*36<=u->bytes.size()-(off-u->offset);
        if(inRange){validation=Validate(u->bytes.data()+off-u->offset,count);reject=validation.position||validation.colour;
            if(DiagnosticOutput && ((!validation.position && validation.colour) || (!reject && (validation.maxPosition>10 || validation.maxColour>100))))
                SaveFiniteSample(c,u->bytes.data()+off-u->offset,count,off,validation.colour || validation.maxColour>100);}
        ReleaseSRWLockShared(&u->lock);u->Release();}
    b->Release();
    ReportCoverage(known,inRange,count,start,stride,off,validation);
    if(DiagnosticOutput && reject){
        ID3D11VertexShader* vs{};ID3D11PixelShader* ps{};
        c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);
        const bool original=Tagged(vs,1)&&Tagged(ps,2);if(vs)vs->Release();if(ps)ps->Release();
        if(!original && g_otherShaderRejected.fetch_add(1)<8){char line[160];sprintf_s(line,"[LIGHTNING] shared-buffer rejection on different shader pair count=%u offset=%u",count,off);RetailLogLine(line);}
    }
    if(DiagnosticOutput && reject && !g_rejected.exchange(true))RetailLogLine("[LIGHTNING] rejected malformed captured-pair geometry from CPU upload; valid draws retained");
    if(reject) ++g_rejectedDraws;
    return reject;
}
}
