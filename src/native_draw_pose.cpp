#include "research_diagnostics.h"
#include "native_draw_pose.h"
#include "runtime_log.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
namespace NativeDrawPose {
namespace {
constexpr GUID kStamp={0xc7eb20b1,0x8bba,0x4287,{0x95,0x10,0x67,0x2e,0x1c,0x45,0x82,0x18}};
constexpr GUID kBatch={0xc7eb20b2,0x8bba,0x4287,{0x95,0x10,0x67,0x2e,0x1c,0x45,0x82,0x18}};
constexpr GUID kRawCam={0xc7eb20b3,0x8bba,0x4287,{0x95,0x10,0x67,0x2e,0x1c,0x45,0x82,0x18}};
constexpr GUID kCameraSlots={0xc7eb20b4,0x8bba,0x4287,{0x95,0x10,0x67,0x2e,0x1c,0x45,0x82,0x18}};
std::atomic_uint64_t g_shaderKnown{},g_shaderUnknown{},g_shaderNoCamera{};
std::atomic_uint64_t g_unusedCameraBindings{};
struct RawCam { float pv[32]; };
struct Stamp { Pose pose{}; ID3D11DeviceContext* writer{}; std::uint64_t cycle{},time{},execution{}; };
struct Batch { Pose pose{}; std::uint64_t cycle{}; bool conflict{}; unsigned executions{}; };
struct Mapping { ID3D11DeviceContext* context{}; ID3D11Resource* resource{}; void* data{}; std::uint64_t cycle{}; };
SRWLOCK g_lock=SRWLOCK_INIT;
struct Guard { Guard(){AcquireSRWLockExclusive(&g_lock);} ~Guard(){ReleaseSRWLockExclusive(&g_lock);} };
std::array<Mapping,64> g_maps{};
std::atomic_bool g_active{},g_hooksReady{},g_singleOutput{};
std::atomic_uint64_t g_execution{},g_cycle{1},g_draw{},g_mapsSeen{},g_mapMatches{},g_candidates{},g_overflow{},g_replays{};
std::atomic_uint64_t g_updates[2]{},g_updateViews{},g_updateMatches{},g_partialUpdates{},g_viewCopies{};
std::atomic_uint64_t g_mapCalls{},g_drawCalls{},g_boundViews{};
std::uint64_t g_activeCaptures{};
Matcher g_matcher{}; std::atomic_uint g_width{},g_height{};
thread_local unsigned g_executeDepth{};
Batch g_frame{};
std::uint64_t g_captures{},g_exact{},g_missing{},g_conflicts{};
bool IsViewBufferUncached(ID3D11Resource* resource) {
    if(!resource)return false;
    D3D11_RESOURCE_DIMENSION type{};resource->GetType(&type);
    if(type!=D3D11_RESOURCE_DIMENSION_BUFFER)return false;
    D3D11_BUFFER_DESC d{};static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    return d.ByteWidth==768 && (d.BindFlags&D3D11_BIND_CONSTANT_BUFFER)!=0;
}
// [TYPECACHE] GetType+GetDesc ran on every Map (~3800 a frame,
// ~0.9 ms on the render thread).  Answer from a per-resource cache instead;
// flushed every 600 presents from Initialize so a reused pointer cannot keep a
// stale answer for long.
struct TypeCacheEntry { std::atomic<std::uintptr_t> key{}; std::atomic<int> view{}; };
TypeCacheEntry g_typeCache[1024];
std::atomic<unsigned> g_typeCacheFlushes{};
void FlushTypeCache() { for(auto& e:g_typeCache){e.key.store(0,std::memory_order_relaxed);} }
bool IsViewBuffer(ID3D11Resource* resource) {
    if(!resource)return false;
    const auto key=reinterpret_cast<std::uintptr_t>(resource);
    auto& e=g_typeCache[((key>>4)^(key>>14))&1023];
    if(e.key.load(std::memory_order_acquire)==key)return e.view.load(std::memory_order_relaxed)!=0;
    const bool view=IsViewBufferUncached(resource);
    e.view.store(view?1:0,std::memory_order_relaxed);e.key.store(key,std::memory_order_release);
    return view;
}
void Merge(Batch& into,const Batch& from) {
    if(!from.cycle || from.cycle!=g_cycle.load())return;
    if(into.cycle!=from.cycle)into=Batch{};
    into.cycle=from.cycle;into.conflict|=from.conflict;
    if(!from.pose.serial)return;
    if(into.pose.serial && (into.pose.serial!=from.pose.serial || into.pose.epoch!=from.pose.epoch ||
        into.pose.renderedEye!=from.pose.renderedEye))
        into.conflict=true;
    else into.pose=from.pose;
}
template<class T> bool Read(ID3D11DeviceChild* object,const GUID& key,T& result) {
    UINT bytes=sizeof(result);
    return object && SUCCEEDED(object->GetPrivateData(key,&bytes,&result)) && bytes==sizeof(result);
}
}
void Initialize(Matcher matcher,unsigned width,unsigned height,bool singleOutput) {
    // Called on Present; matcher never changes after initialization.
    static unsigned presents=0; if((++presents%600)==0)FlushTypeCache(); // [TYPECACHE]
    if(!g_matcher)g_matcher=matcher;
    g_singleOutput.store(singleOutput,std::memory_order_relaxed);
    if(g_width!=width || g_height!=height){SetActive(false);g_width=width;g_height=height;}
}
void SetHooksReady(bool ready) { g_hooksReady.store(ready); if(!ready)SetActive(false); }
void RememberVertexShader(ID3D11VertexShader* shader,const void* bytes,SIZE_T length) {
    if(!shader || !bytes || !length)return;
    ID3D11ShaderReflection* reflection{};
    if(FAILED(D3DReflect(bytes,length,__uuidof(ID3D11ShaderReflection),reinterpret_cast<void**>(&reflection))))return;
    D3D11_SHADER_DESC desc{};
    bool known=SUCCEEDED(reflection->GetDesc(&desc));
    UINT slots=0;
    for(UINT i=0;known && i<desc.BoundResources;++i){
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if(FAILED(reflection->GetResourceBindingDesc(i,&binding))){known=false;break;}
        if(binding.Type!=D3D_SIT_CBUFFER || binding.BindPoint>=2)continue;
        auto* buffer=reflection->GetConstantBufferByName(binding.Name);
        D3D11_SHADER_BUFFER_DESC bufferDesc{};
        if(!buffer || FAILED(buffer->GetDesc(&bufferDesc))){known=false;break;}
        for(UINT j=0;j<bufferDesc.Variables;++j){
            auto* variable=buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC v{};
            if(!variable || FAILED(variable->GetDesc(&v))){known=false;break;}
            // The stamped buffer also contains derived camera matrices.
            // A shader may use those without reading the raw view at +64.
            // Exclude only a wholly unused binding, never individual fields.
            if(v.uFlags&D3D_SVF_USED)slots|=1u<<binding.BindPoint;
        }
    }
    reflection->Release();
    if(known)shader->SetPrivateData(kCameraSlots,sizeof(slots),&slots);
}
void SetActive(bool active) {
    active=active && g_hooksReady.load();
    if(g_active.exchange(active)==active)return;
    Guard guard;++g_cycle;g_frame={};g_maps={};g_draw=0;
}
void Invalidate(ID3D11Resource* resource) {
    if(!g_active.load() || !resource)return;
    // Only write metadata when a stamp exists. Texture copies stay cheap.
    Stamp stamp{};
    if(Read(resource,kStamp,stamp)){stamp={};resource->SetPrivateData(kStamp,sizeof(stamp),&stamp);}
}
void Mapped(ID3D11DeviceContext* context,ID3D11Resource* resource,D3D11_MAP mode,void* data) {
    if(g_active.load() && !g_executeDepth)if constexpr(ResearchDiagnostics::Enabled)++g_mapCalls;
    if(g_executeDepth || !g_active.load() || mode==D3D11_MAP_READ || !data || !IsViewBuffer(resource))return;
    Invalidate(resource);
    Guard guard;if constexpr(ResearchDiagnostics::Enabled)++g_mapsSeen;
    for(auto& map:g_maps)if(!map.resource){map={context,resource,data,g_cycle.load()};return;}
    if constexpr(ResearchDiagnostics::Enabled)++g_overflow;
}
void Unmapping(ID3D11DeviceContext* context,ID3D11Resource* resource) {
    if(g_executeDepth || !g_active.load())return;
    Mapping map{};
    {Guard guard;for(auto& candidate:g_maps)
        if(candidate.context==context && candidate.resource==resource){map=candidate;candidate={};break;}}
    if(!map.data || map.cycle!=g_cycle.load())return;
    // Read after any outer game/proxy edits, immediately before real Unmap.
    // The observed view CB has P at 0x00 and V at 0x40. No fuzzy matching.
    alignas(16) float view[16]{};
    std::memcpy(view,static_cast<const char*>(map.data)+64,sizeof(view));
    Stamp stamp{};stamp.writer=context;stamp.cycle=map.cycle;stamp.time=GetTickCount64();stamp.execution=g_execution.load();
    if(g_matcher && g_matcher(view,stamp.pose)){if constexpr(ResearchDiagnostics::Enabled)++g_mapMatches;}
    else stamp.pose={};
    resource->SetPrivateData(kStamp,sizeof(stamp),&stamp);
}

void Updated(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,
    const D3D11_BOX* box,const void* data,bool version1) {
    // [GBUF] raw P (0x00) + V (0x40) of every whole camera-buffer upload, independent of g_active.
    if(ResearchDiagnostics::Enabled && !g_executeDepth && data && !box && subresource==0 && IsViewBuffer(resource)){
        RawCam raw{};std::memcpy(raw.pv,data,sizeof(raw.pv));
        resource->SetPrivateData(kRawCam,sizeof(raw),&raw);
    }
    if(g_executeDepth || !g_active.load())return;
    ++g_updates[version1?1:0];
    Invalidate(resource);
    if(!IsViewBuffer(resource))return;
    if constexpr(ResearchDiagnostics::Enabled)++g_updateViews;
    // Capture only whole camera-buffer uploads. Partial writes fail closed;
    // no inference about the untouched bytes or driver box-offset workarounds.
    if(subresource!=0 || box || !data){if constexpr(ResearchDiagnostics::Enabled)++g_partialUpdates;return;}
    alignas(16) float view[16]{};
    std::memcpy(view,static_cast<const char*>(data)+64,sizeof(view));
    Stamp stamp{};stamp.writer=context;stamp.cycle=g_cycle.load();stamp.time=GetTickCount64();stamp.execution=g_execution.load();
    if(g_matcher && g_matcher(view,stamp.pose)){if constexpr(ResearchDiagnostics::Enabled)++g_updateMatches;}
    else stamp.pose={};
    resource->SetPrivateData(kStamp,sizeof(stamp),&stamp);
}
void Copied(ID3D11Resource* resource) {
    if(g_executeDepth || !g_active.load())return;
    if(IsViewBuffer(resource))if constexpr(ResearchDiagnostics::Enabled)++g_viewCopies;
    Invalidate(resource);
}

bool ReadStamp(ID3D11DeviceContext* context,ID3D11Buffer* buffer,Pose& pose) {
    Stamp stamp{};
    if(!g_active.load() || !Read(buffer,kStamp,stamp) || !stamp.pose.serial ||
        stamp.writer!=context || stamp.cycle!=g_cycle.load() || stamp.execution!=g_execution.load() ||
        GetTickCount64()-stamp.time>1000)return false;
    pose=stamp.pose;return true;
}
void BeforeDraw(ID3D11DeviceContext* context) {
    if(g_executeDepth || !g_active.load() || !context)return;
    // Bounded binding inspection, distributed over the real draw stream.
    // This does not claim coverage of every draw or final-image ownership.
    if constexpr(ResearchDiagnostics::Enabled)++g_drawCalls;
    const auto ordinal=g_draw.fetch_add(1,std::memory_order_relaxed);
    // Single-output world passes can begin after 8192 shadow/effect draws.
    // Keep the same sampling density through the whole stream in that mode.
    if(ordinal%128 || (!g_singleOutput.load(std::memory_order_relaxed) && ordinal>=8192))return;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT n=_countof(vp);context->RSGetViewports(&n,vp);
    if(n!=1 || vp[0].TopLeftX!=0 || vp[0].TopLeftY!=0 ||
        vp[0].Width!=float(g_width) || vp[0].Height!=float(g_height))return;
    UINT slots=3;
    if(g_singleOutput.load(std::memory_order_relaxed)){
        ID3D11VertexShader* shader{};context->VSGetShader(&shader,nullptr,nullptr);
        if(Read(shader,kCameraSlots,slots))++g_shaderKnown;
        else {slots=3;++g_shaderUnknown;}
        if(shader)shader->Release();
        if(!slots){++g_shaderNoCamera;return;}
    }
    ID3D11Buffer* buffers[2]{};UINT first[2]{},count[2]{4096,4096};
    ID3D11DeviceContext1* context1{};
    if(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&context1)))){
        context1->VSGetConstantBuffers1(0,2,buffers,first,count);context1->Release();
    } else context->VSGetConstantBuffers(0,2,buffers);
    Batch batch{};batch.cycle=g_cycle.load();
    for(unsigned i=0;i<2;++i){
        if(IsViewBuffer(buffers[i]))if constexpr(ResearchDiagnostics::Enabled)++g_boundViews;
        Pose pose{};
        if(buffers[i] && first[i]==0 && count[i]>=8 && ReadStamp(context,buffers[i],pose)){
            if(slots&(1u<<i))Merge(batch,Batch{pose,batch.cycle,false,0});
            else ++g_unusedCameraBindings;
        }
        if(buffers[i])buffers[i]->Release();
    }
    if(!batch.pose.serial && !batch.conflict)return;
    if constexpr(ResearchDiagnostics::Enabled)++g_candidates;
    Guard guard;
    if(context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)Merge(g_frame,batch);
    else {
        Batch recorded{};Read(context,kBatch,recorded);Merge(recorded,batch);
        context->SetPrivateData(kBatch,sizeof(recorded),&recorded);
    }
}
void Finished(ID3D11DeviceContext* context,ID3D11CommandList* list) {
    if(!g_active.load() || !list)return;
    Guard guard;Batch batch{};Read(context,kBatch,batch);
    if(batch.cycle==g_cycle.load())list->SetPrivateData(kBatch,sizeof(batch),&batch);
    batch={};context->SetPrivateData(kBatch,sizeof(batch),&batch);
}
void BeginExecute() { ++g_executeDepth; ++g_execution; }
bool InsideExecute() { return g_executeDepth!=0; }
void Executed(ID3D11CommandList* list) {
    if(g_executeDepth)--g_executeDepth;
    if(!g_active.load() || !list)return;
    Guard guard;Batch batch{};
    if(!Read(list,kBatch,batch) || batch.cycle!=g_cycle.load())return;
    if(++batch.executions>1){batch.conflict=true;if constexpr(ResearchDiagnostics::Enabled)++g_replays;}
    list->SetPrivateData(kBatch,sizeof(batch),&batch);
    Merge(g_frame,batch);
}
bool ReadRawCamera(ID3D11DeviceContext* context,float* out) {
    if(!context || !out)return false;
    ID3D11Buffer* buffers[2]{};
    context->VSGetConstantBuffers(0,2,buffers);
    bool found=false;
    for(unsigned i=0;i<2 && !found;++i){
        RawCam raw{};
        if(buffers[i] && IsViewBuffer(buffers[i]) && Read(buffers[i],kRawCam,raw)){
            std::memcpy(out,raw.pv,sizeof(raw.pv));found=true;
        }
    }
    for(auto* b:buffers)if(b)b->Release();
    return found;
}
Pose Pending() {
    Guard guard;
    if(g_active.load() && g_frame.cycle==g_cycle.load() && !g_frame.conflict)return g_frame.pose;
    return {};
}
Pose CaptureBoundary() {
    Guard guard;Pose result{};
    if(g_active.load())++g_activeCaptures;
    if(g_active.load() && g_frame.cycle==g_cycle.load() && !g_frame.conflict)result=g_frame.pose;
    if(result.serial)++g_exact;else if(g_frame.conflict)++g_conflicts;else ++g_missing;
    const auto drawCount=g_draw.load(std::memory_order_relaxed);
    if(g_singleOutput.load(std::memory_order_relaxed)){
        static std::uint64_t captures{},exact{},missing{},conflicts{},longFrames{},maxDraws{};
        ++captures;
        if(result.serial)++exact;else if(g_frame.conflict)++conflicts;else ++missing;
        if(drawCount>8192)++longFrames;
        if(drawCount>maxDraws)maxDraws=drawCount;
        if(captures%600==0){
            char line[256]{};
            std::snprintf(line,sizeof(line),"[SINGLE_POSE_COVERAGE] exact/missing/conflict=%llu/%llu/%llu drawsOver8192=%llu maxDraws=%llu",
                exact,missing,conflicts,longFrames,maxDraws);
            RetailLogLine(line);
            std::snprintf(line,sizeof(line),"[SINGLE_POSE_SHADER] sampled known/unknown/noCamera=%llu/%llu/%llu unusedCameraBindings=%llu",
                g_shaderKnown.exchange(0),g_shaderUnknown.exchange(0),g_shaderNoCamera.exchange(0),g_unusedCameraBindings.exchange(0));
            RetailLogLine(line);
            exact=missing=conflicts=longFrames=maxDraws=0;
        }
    }
    g_frame={};g_draw=0;
    if(ResearchDiagnostics::Enabled && (++g_captures%300==0 || g_captures<=3)){
        char route[600]{};
        std::snprintf(route,sizeof(route),
            "[DRAWPOSE_ROUTE] active=%u hooksReady=%u activeCaptures=%llu drawCalls=%llu mapCalls=%llu "
            "updates/updates1=%llu/%llu update768/matched/partial=%llu/%llu/%llu copy768=%llu bound768=%llu",
            unsigned(g_active.load()),unsigned(g_hooksReady.load()),g_activeCaptures,g_drawCalls.load(),g_mapCalls.load(),
            g_updates[0].load(),g_updates[1].load(),g_updateViews.load(),g_updateMatches.load(),
            g_partialUpdates.load(),g_viewCopies.load(),g_boundViews.load());
        RetailLogLine(route);
        char line[400]{};
        std::snprintf(line,sizeof(line),"[DRAWPOSE] captures=%llu selected/missing/conflict=%llu/%llu/%llu "
            "map768/matched=%llu/%llu sampledCandidates=%llu mapOverflow=%llu replay=%llu; "
            "serial=%llu epoch=%llu (draw-bound evidence, not final-pixel proof)",
            g_captures,g_exact,g_missing,g_conflicts,g_mapsSeen.load(),g_mapMatches.load(),
            g_candidates.load(),g_overflow.load(),g_replays.load(),result.serial,result.epoch);
        RetailLogLine(line);
    }
    return result;
}
}
