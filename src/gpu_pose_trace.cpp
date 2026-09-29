#include "gpu_pose_trace.h"
#include "native_draw_pose.h"
#include "runtime_log.h"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace GpuPoseTrace
{
namespace
{
constexpr unsigned kBytes=4096, kSlots=600, kFrames=2400;
struct Binding { unsigned bytes{}, first{}, count{}; };
constexpr GUID kTicketsGuid={0xd45131b9,0x7245,0x4f70,{0xb9,0x57,0xcb,0x7d,0x7f,0x12,0xf5,0xaa}};
struct Sample
{
    ID3D11Buffer* staging{};
    ID3D11Query* event{};
    ID3D11DeviceContext* context{}; // retained for the bounded process-lifetime trace
    std::uint64_t capture{}, recordingFrame{};
    unsigned draw{}, cbSlot{}, executions{};
    std::uint64_t cpuStamp{};
    bool issued{}, attached{}, ready{}, matched{}, transposed{};
    Binding bindings[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    unsigned bufferBytes{}, firstConstant{}, numConstants{}, copiedBytes{}, boundCount{}, matchOffset{};
    bool rangeApi{};
    std::uintptr_t shader{};
    float viewport[4]{};
    alignas(16) float payload[kBytes/4]{};
    HRESULT readResult{S_FALSE};
    Match match{};
    float view[16]{};
};
struct Frame
{
    XrQuaternionf submitted{};
    std::uint64_t associated{}, latest{}, epoch{}, direct{};
    bool assigned{};
};
struct Tickets { unsigned count{}; unsigned slot[64]{}; };
SRWLOCK g_lock=SRWLOCK_INIT;
struct Guard { Guard(){AcquireSRWLockExclusive(&g_lock);} ~Guard(){ReleaseSRWLockExclusive(&g_lock);} };
std::array<Sample,kSlots> g_samples{};
std::array<Frame,kFrames+121> g_frames{};
ID3D11DeviceContext* g_immediate{};
ID3D11Device* g_device{};
Matcher g_matcher{};
#ifdef GPU_POSE_TRACE_TEST
HRESULT g_testQuery=E_PENDING,g_testMap=E_PENDING;BOOL g_testComplete=FALSE;
#endif
std::atomic_bool g_enabled{},g_active{},g_native{},g_finished{},g_initialized{};
std::atomic_uint64_t g_frame{};
std::atomic_uint g_draw{},g_attempts{};
unsigned g_count{},g_budgetDrops{},g_noBuffer{},g_untrackedLists{},g_captureCount{};
std::atomic_uint64_t g_drawCalls{};
bool g_failed{};
std::uint64_t g_lastReservedFrame{UINT64_MAX};
wchar_t g_output[MAX_PATH]{};
struct Report
{
    std::array<Sample,kSlots> samples;
    std::array<Frame,kFrames+121> frames;
    unsigned count,noBuffer,budgetDrops,untrackedLists;
    std::uint64_t drawCalls;
};

double Angle(const XrQuaternionf& a,const XrQuaternionf& b)
{
    const double aa=double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z+double(a.w)*a.w;
    const double bb=double(b.x)*b.x+double(b.y)*b.y+double(b.z)*b.z+double(b.w)*b.w;
    if(aa<=0 || bb<=0) return -1;
    const double dot=std::abs(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w);
    return 2*std::acos(std::clamp(dot/std::sqrt(aa*bb),0.0,1.0))*180/3.141592653589793;
}
DWORD WINAPI WriteCsv(void* argument)
{
    std::unique_ptr<Report> report(static_cast<Report*>(argument));
    FILE* file{};
    if(_wfopen_s(&file,g_output,L"wb")!=0 || !file)
    { RetailLogLine("[GPUPOSE] could not write head_frame_trace.csv"); return 0; }
    std::fputs("capture,recording_frame,draw,cb_slot,executions,ready,hr,gpu_match,gpu_serial,epoch,transpose,pose_assigned,associated_serial,located_serial,angular_error_deg,view,frame_epoch,gpu_quaternion,assigned_quaternion,bound_count,buffer_bytes,first_constant,num_constants,copied_bytes,range_api,match_offset,shader,viewport,bindings,payload_hex_words,cpu_bound_serial,direct_frame_serial\n",file);
    unsigned matched=0,exact=0,different=0,unknown=0;
    for(unsigned i=0;i<report->count;++i)
    {
        const auto& s=report->samples[i];
        const Frame f=s.capture<report->frames.size()?report->frames[s.capture]:Frame{};
        const bool usable=s.ready&&s.matched&&s.executions==1&&s.capture&&f.assigned&&s.match.epoch==f.epoch;
        const double error=usable?Angle(s.match.orientation,f.submitted):-1;
        if(usable){++matched;if(s.match.serial==f.associated)++exact;else ++different;}
        else ++unknown;
        std::fprintf(file,"%llu,%llu,%u,%u,%u,%u,0x%08lX,%u,%llu,%llu,%u,%u,%llu,%llu,%.6f,\"",
            s.capture,s.recordingFrame,s.draw,s.cbSlot,s.executions,s.ready?1:0,
            static_cast<unsigned long>(s.readResult),s.matched?1:0,s.match.serial,s.match.epoch,
            s.transposed?1:0,f.assigned?1:0,f.associated,f.latest,error);
        for(unsigned j=0;j<16;++j) std::fprintf(file,j?" %.9g":"%.9g",s.view[j]);
        std::fprintf(file,"\",%llu,\"%.9g %.9g %.9g %.9g\",\"%.9g %.9g %.9g %.9g\",%u,%u,%u,%u,%u,%u,%u,%llX,\"%.1f %.1f %.1f %.1f\",\"",f.epoch,
            s.match.orientation.x,s.match.orientation.y,s.match.orientation.z,s.match.orientation.w,
            f.submitted.x,f.submitted.y,f.submitted.z,f.submitted.w,
            s.boundCount,s.bufferBytes,s.firstConstant,s.numConstants,s.copiedBytes,s.rangeApi?1:0,s.matchOffset,
            static_cast<unsigned long long>(s.shader),s.viewport[0],s.viewport[1],s.viewport[2],s.viewport[3]);
        for(unsigned j=0;j<_countof(s.bindings);++j){const auto& b=s.bindings[j];std::fprintf(file,j?";%u:%u:%u:%u":"%u:%u:%u:%u",j,b.bytes,b.first,b.count);}
        std::fputs("\",\"",file);
        for(unsigned j=0;j<s.copiedBytes/4;++j){unsigned word{};std::memcpy(&word,&s.payload[j],4);std::fprintf(file,j?" %08X":"%08X",word);}
        std::fprintf(file,"\",%llu,%llu\n",s.cpuStamp,f.direct);
    }
    std::fclose(file);
    char line[384]{};
    std::snprintf(line,sizeof(line),"[GPUPOSE] trace complete: samples=%u usable=%u GPU-vs-FIFO same/different=%u/%u unknown=%u noVSConstantBuffer=%u dropped=%u untrackedLists=%u; head_frame_trace.csv. Draw-constant evidence, not headset delivery or final-pixel coverage.",
        report->count,matched,exact,different,unknown,report->noBuffer,report->budgetDrops,report->untrackedLists);
    RetailLogLine(line);
    std::snprintf(line,sizeof(line),"[GPUPOSE] observed draw-hook calls=%llu; timestamped output saved (draw-hook coverage must be checked against game workload).",report->drawCalls);
    RetailLogLine(line);
    return 0;
}
void Finish()
{
    if(g_finished.exchange(true)) return;
    g_active.store(false);
    auto* report=new(std::nothrow) Report;
    if(!report){RetailLogLine("[GPUPOSE] report allocation failed");return;}
    {Guard guard;report->samples=g_samples;report->frames=g_frames;
        report->count=g_count;report->noBuffer=g_noBuffer;report->budgetDrops=g_budgetDrops;report->untrackedLists=g_untrackedLists;report->drawCalls=g_drawCalls.load();}
    // Only the immutable snapshot is read by the IO worker.
    if(HANDLE worker=CreateThread(nullptr,0,WriteCsv,report,0,nullptr))CloseHandle(worker);
    else {delete report;RetailLogLine("[GPUPOSE] CSV worker creation failed");}
}
}
void SetEnabled(bool enabled){g_enabled.store(enabled);}
void Initialize(ID3D11Device* device,ID3D11DeviceContext* immediate,Matcher matcher)
{
    if(!g_enabled.load()||g_initialized||g_failed||!device||!immediate)return;
    // Initialize on Present, before activating draw capture. Fixed pool never
    // reuses targets: replayed deferred lists cannot overwrite a newer sample.
    for(auto& s:g_samples)
    {
        D3D11_BUFFER_DESC d{};d.ByteWidth=kBytes;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
        if(FAILED(device->CreateBuffer(&d,nullptr,&s.staging))||FAILED(device->CreateQuery(&q,&s.event)))
        {
            g_failed=true;
            for(auto& old:g_samples){if(old.staging){old.staging->Release();old.staging=nullptr;}if(old.event){old.event->Release();old.event=nullptr;}}
            RetailLogLine("[GPUPOSE] allocation failed; trace disabled");return;
        }
    }
    g_device=device;device->AddRef();g_immediate=immediate;immediate->AddRef();g_matcher=matcher;
    GetModuleFileNameW(nullptr,g_output,MAX_PATH);
    if(auto* slash=wcsrchr(g_output,L'\\'))slash[1]=0;else g_output[0]=0;
    SYSTEMTIME time{};GetLocalTime(&time);wchar_t name[100]{};
    swprintf_s(name,L"head_frame_trace_%04u%02u%02u_%02u%02u%02u_%lu.csv",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
    wcscat_s(g_output,name);
    g_initialized=true;
    RetailLogLine("[GPUPOSE] armed v3: all VS constant-buffer sizes/slots/ranges inventoried; <=1 bound-range readback (4 KiB max) per 4 captures; 2400 captures max; explicit no-binding rows; no GPU waits/flushes.");
}
void SetNativeActive(bool active)
{
    const bool previous=g_native.exchange(active);
    if(previous&&!active&&g_frame.load())Finish();
    g_active.store(active&&g_enabled.load()&&g_initialized&&!g_finished.load()&&g_frame.load()<kFrames);
}
void BeforeDraw(ID3D11DeviceContext* context)
{
    if(NativeDrawPose::InsideExecute()||!g_active.load(std::memory_order_acquire)||!context)return;
    g_drawCalls.fetch_add(1,std::memory_order_relaxed);
    const auto frame=g_frame.load(std::memory_order_relaxed);
    if(frame%4)return;
    const unsigned draw=g_draw.fetch_add(1,std::memory_order_relaxed);
    // Previous sampling only visited the first 512 draws. Walk the broader
    // draw stream (live run averaged ~8900 calls per capture).
    if(draw!=((unsigned(frame/4)*137u)%4096u))return;
    if(g_attempts.fetch_add(1,std::memory_order_relaxed)>=8)return;
    {
        Guard guard;
        if(g_lastReservedFrame==frame||g_finished.load())return;
    }
    ID3D11Device* owner{};context->GetDevice(&owner);
    const bool sameDevice=owner==g_device;if(owner)owner->Release();
    if(!sameDevice)return;
    ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    UINT first[_countof(buffers)]{},counts[_countof(buffers)]{};
    std::fill(std::begin(counts),std::end(counts),4096u);
    ID3D11DeviceContext1* context1{};
    const bool rangeApi=SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&context1)))&&context1;
    if(rangeApi){context1->VSGetConstantBuffers1(0,_countof(buffers),buffers,first,counts);context1->Release();}
    else context->VSGetConstantBuffers(0,_countof(buffers),buffers);
    Binding bindings[_countof(buffers)]{};
    unsigned boundCount=0;
    for(unsigned i=0;i<_countof(buffers);++i)
    {
        if(!buffers[i])continue;
        D3D11_BUFFER_DESC d{};buffers[i]->GetDesc(&d);
        bindings[i]={d.ByteWidth,first[i],counts[i]};++boundCount;
    }
    unsigned slot=UINT_MAX,ordinal=0;
    const unsigned selected=boundCount?unsigned(frame/4)%boundCount:0;
    for(unsigned i=0;i<_countof(buffers);++i)if(buffers[i]&&ordinal++==selected){slot=i;break;}
    unsigned index=kSlots;
    {
        Guard guard;
        if(g_lastReservedFrame!=frame&&g_frame.load()==frame&&!g_finished.load())
        {
            if(g_count<kSlots)
            {
                index=g_count++;g_lastReservedFrame=frame;
                auto& s=g_samples[index];
                s.context=context;context->AddRef();s.recordingFrame=frame;s.draw=draw;s.cbSlot=slot;
                s.boundCount=boundCount;s.rangeApi=rangeApi;std::memcpy(s.bindings,bindings,sizeof(bindings));
                if(slot!=UINT_MAX){const auto& b=bindings[slot];s.bufferBytes=b.bytes;s.firstConstant=b.first;s.numConstants=b.count;
                    const std::uint64_t offset=std::uint64_t(b.first)*16;
                    if(offset<b.bytes)s.copiedBytes=static_cast<unsigned>(std::min({std::uint64_t(kBytes),std::uint64_t(b.bytes)-offset,std::uint64_t(b.count)*16}));}
                else ++g_noBuffer;
            }
            else ++g_budgetDrops;
        }
    }
    if(index==kSlots){for(auto* b:buffers)if(b)b->Release();return;}
    auto& s=g_samples[index];
    NativeDrawPose::Pose cpu{};
    if(slot!=UINT_MAX && first[slot]==0 && NativeDrawPose::ReadStamp(context,buffers[slot],cpu))
        s.cpuStamp=cpu.serial;
    ID3D11VertexShader* shader{};context->VSGetShader(&shader,nullptr,nullptr);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};UINT viewportCount=_countof(viewports);
    context->RSGetViewports(&viewportCount,viewports);
    {Guard guard;s.shader=reinterpret_cast<std::uintptr_t>(shader);if(viewportCount){s.viewport[0]=viewports[0].TopLeftX;s.viewport[1]=viewports[0].TopLeftY;s.viewport[2]=viewports[0].Width;s.viewport[3]=viewports[0].Height;}}
    if(shader)shader->Release();
    if(s.copiedBytes){const UINT offset=s.firstConstant*16;const D3D11_BOX box{offset,0,0,offset+s.copiedBytes,1,1};
        context->CopySubresourceRegion(s.staging,0,0,0,0,buffers[slot],0,&box);context->End(s.event);}
    for(auto* b:buffers)if(b)b->Release();
    const bool immediate=context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;
    {Guard guard;s.issued=true;if(immediate)s.executions=1;if(!s.copiedBytes){s.ready=true;s.readResult=S_FALSE;}}
}
void Finished(ID3D11DeviceContext* context,ID3D11CommandList* list)
{
    if(!g_initialized||g_finished.load()||!list)return;
    Tickets tickets{};
    {
        Guard guard;
        for(unsigned i=0;i<g_count;++i)
        {
            auto& s=g_samples[i];
            if(s.context!=context||!s.issued||s.attached||s.executions)continue;
            s.attached=true;
            if(tickets.count<_countof(tickets.slot))tickets.slot[tickets.count++]=i;
            else ++g_untrackedLists;
        }
    }
    if(tickets.count&&FAILED(list->SetPrivateData(kTicketsGuid,sizeof(tickets),&tickets)))
    {Guard guard;++g_untrackedLists;}
}
void Executed(ID3D11CommandList* list)
{
    if(!g_initialized||g_finished.load()||!list)return;
    Tickets tickets{};UINT size=sizeof(tickets);
    if(FAILED(list->GetPrivateData(kTicketsGuid,&size,&tickets))||size!=sizeof(tickets)||tickets.count>_countof(tickets.slot))return;
    Guard guard;
    for(unsigned i=0;i<tickets.count;++i)
        if(tickets.slot[i]<g_count)++g_samples[tickets.slot[i]].executions;
}
std::uint64_t CaptureBoundary()
{
    if(!g_initialized||!g_native.load()||g_finished.load())return 0;
    Guard guard;
    // During drain, advance only while native mode remains in use (caller gate).
    const auto frame=g_frame.fetch_add(1)+1;
    ++g_captureCount;g_draw.store(0);g_attempts.store(0);
    for(unsigned i=0;i<g_count;++i)
    {auto& s=g_samples[i];if(s.issued&&s.executions&&!s.capture)s.capture=frame;}
    if(frame>=kFrames)g_active.store(false);
    return frame;
}
void PoseAssigned(std::uint64_t capture,const XrPosef& pose,std::uint64_t associated,std::uint64_t latest,std::uint64_t epoch,std::uint64_t direct)
{
    if(!capture||capture>=g_frames.size()||g_finished.load())return;
    Guard guard;g_frames[capture]={pose.orientation,associated,latest,epoch,direct,true};
}
void Poll()
{
    if(!g_initialized||g_finished.load())return;
    unsigned count;
    {Guard guard;count=g_count;}
    for(unsigned i=0;i<count;++i)
    {
        bool pending;
        {Guard guard;const auto& s=g_samples[i];pending=s.issued&&s.executions==1&&s.capture&&!s.ready;}
        if(!pending)continue;
        auto& s=g_samples[i];BOOL complete=FALSE;
        const HRESULT query=g_immediate->GetData(s.event,&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);
#ifdef GPU_POSE_TRACE_TEST
        g_testQuery=query;g_testComplete=complete;
#endif
        if(query==S_FALSE||(query==S_OK&&!complete))continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT result=FAILED(query)?query:g_immediate->Map(s.staging,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
#ifdef GPU_POSE_TRACE_TEST
        g_testMap=result;
#endif
        if(result==DXGI_ERROR_WAS_STILL_DRAWING)continue;
        Match match{};bool found=false,transpose=false;float view[16]{};alignas(16) float payload[kBytes/4]{};unsigned matchOffset=0;
        if(result==S_OK)
        {
            std::memcpy(payload,map.pData,s.copiedBytes);
            g_immediate->Unmap(s.staging,0);
            // Exact matching only; raw bytes retained even if no known camera
            // matrix exists. Scan 64-byte-aligned matrices within captured range.
            for(unsigned at=0;g_matcher&&at+64<=s.copiedBytes&&!found;at+=64)
            {
                std::memcpy(view,reinterpret_cast<const char*>(payload)+at,64);
                found=g_matcher(view,match);
                if(found){matchOffset=at;break;}
                float transposed[16]{};
                for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)transposed[4*r+c]=view[4*c+r];
                found=g_matcher(transposed,match);transpose=found;if(found)matchOffset=at;
            }
        }
        {Guard guard;s.readResult=result;s.ready=true;s.matched=found;s.transposed=transpose;s.match=match;std::memcpy(s.view,view,sizeof(view));
            s.matchOffset=matchOffset;std::memcpy(s.payload,payload,sizeof(payload));}
    }
    const auto frame=g_frame.load();
    if(frame>=kFrames)
    {
        bool pending=false;
        {Guard guard;for(unsigned i=0;i<g_count;++i){const auto& s=g_samples[i];pending|=s.issued&&s.executions==1&&!s.ready;}}
        if(!pending||frame>=kFrames+120)Finish();
    }
}
#ifdef GPU_POSE_TRACE_TEST
TestResult Inspect()
{
    Guard guard;TestResult r{};r.samples=g_count;r.query=g_testQuery;r.map=g_testMap;r.complete=g_testComplete;
    for(unsigned i=0;i<g_count;++i){const auto& s=g_samples[i];r.captured+=s.capture!=0;r.ready+=s.ready;r.matched+=s.matched;r.executed+=s.executions!=0;r.replayed+=s.executions>1;if(s.matched){r.serial=s.match.serial;r.capture=s.capture;}if(s.ready&&s.readResult==S_OK){++r.readbacks;r.firstValue=s.payload[0];r.bytes=s.bufferBytes;r.copied=s.copiedBytes;r.first=s.firstConstant;r.slot=s.cbSlot;}if(!s.boundCount)++r.empty;}
    return r;
}
#endif
}
