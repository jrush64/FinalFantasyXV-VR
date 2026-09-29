#pragma once
#include "ui_world_probe.h"
#include <atomic>
#include <cstdint>
#include <cstring>
namespace RemainingUiProbe {
#pragma pack(push,1)
struct Record {
    std::uint32_t present,copy,ordinal,kind,lane,count,start,stride;
    std::uint32_t contextType,cb0Bytes,cb1Bytes,vertexBytes,indexBytes,indexFormat,firstVertex,flags;
    std::uint64_t ticks,vs,ps,texture;
    std::uint32_t texWidth,texHeight,texFormat;
    std::int32_t baseVertex;
    float viewport[6],cb0[136],cb1[64];
    unsigned char vertices[192],indices[48];
};
#pragma pack(pop)
static_assert(sizeof(Record)==1176);
inline constexpr unsigned Capacity=4096;
inline Record records[Capacity]{},pending[Capacity]{};
inline SRWLOCK lock=SRWLOCK_INIT;
inline std::atomic_bool enabled{},sampling{};
inline unsigned count{},generation{},sampleFrame{};
inline std::atomic_uint overflow{},contention{},filtered{};
inline bool checked{};
inline HANDLE file=INVALID_HANDLE_VALUE;
inline UiWorldProbe::LogFn logger{};
inline bool Sampling(){if constexpr (!ResearchDiagnostics::MarkerCapture) return false; return sampling.load(std::memory_order_acquire);}
// Early untextured 40-byte draws exhausted every snapshot before
// SWF markers. Exclude only that measured signature; keep all textured fonts,
// unknown sprite layouts and every recognized marker, even with missing CBs.
inline bool Relevant(const Record& r) {
    return !(r.kind==0 && r.stride==40 && r.cb0Bytes==0 && r.texWidth==1 && r.texHeight==1);
}
inline void Append(const Record& record) {
    if(!Sampling())return;
    if(!Relevant(record)){++filtered;return;}
    if(!TryAcquireSRWLockExclusive(&lock)){++contention;return;}
    if(Sampling()) {if(count<Capacity)records[count++]=record;else ++overflow;}
    ReleaseSRWLockExclusive(&lock);
}
inline void Flush() {
    sampling.store(false,std::memory_order_release);
    AcquireSRWLockExclusive(&lock);const auto n=count;std::memcpy(pending,records,n*sizeof(Record));count=0;ReleaseSRWLockExclusive(&lock);
    DWORD bytes{};const bool ok=UiWorldProbe::rolling ? UiWorldProbe::WriteRing(L"ui",generation,pending,n*sizeof(Record),sizeof(Record),"FFXVUI01") :
        WriteFile(file,pending,n*sizeof(Record),&bytes,nullptr) && bytes==n*sizeof(Record);
    if(logger)logger("[UIREMAIN] window=%u frame=%u draws=%u overflow=%u contention=%u filtered=%u write=%s",generation,sampleFrame,n,overflow.exchange(0),contention.exchange(0),filtered.exchange(0),ok?"ok":"failed");
    if(!ok){enabled=false;CloseHandle(file);file=INVALID_HANDLE_VALUE;}
}
inline void Present(unsigned frame,UiWorldProbe::LogFn log) {
    if constexpr (!ResearchDiagnostics::MarkerCapture) return;
    if(!checked) {
        checked=true;logger=log;wchar_t executable[MAX_PATH]{};
        if(!GetModuleFileNameW(nullptr,executable,MAX_PATH))return;
        auto* slash=wcsrchr(executable,L'\\');if(!slash)return;slash[1]=0;
        wchar_t once[MAX_PATH]{},output[MAX_PATH]{};
        swprintf_s(once,L"%sffxv_remaining_ui.once",executable);
        if(GetFileAttributesW(once)==INVALID_FILE_ATTRIBUTES)return;
        // Projection capture must be active through the existing guarded provider.
        if(!UiWorldProbe::enabled.load())return;
        swprintf_s(output,L"%sffxv_remaining_ui_%lu.bin",executable,GetCurrentProcessId());
        file=CreateFileW(output,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        LARGE_INTEGER freq{};QueryPerformanceFrequency(&freq);
        UiWorldProbe::Header header{{'F','F','X','V','U','I','0','1'},1,sizeof(Record),static_cast<std::uint64_t>(freq.QuadPart)};
        DWORD bytes{};
        if(!WriteFile(file,&header,sizeof(header),&bytes,nullptr) || bytes!=sizeof(header) || !DeleteFileW(once)) {CloseHandle(file);file=INVALID_HANDLE_VALUE;return;}
        if(UiWorldProbe::rolling){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
        enabled=true;if(log)log("[UIREMAIN] one-run read-only UI capture armed; first present of each projection window, max 4096 draws/window; excludes measured untextured early draws; no rendering changes");
    }
    if(!enabled)return;
    if(Sampling() && frame!=sampleFrame)Flush();
    if(!enabled)return;
    if(!UiWorldProbe::enabled.load()) {enabled=false;CloseHandle(file);file=INVALID_HANDLE_VALUE;return;}
    const unsigned next=UiWorldProbe::generation.load(std::memory_order_acquire);
    if(next && next!=generation){generation=next;sampleFrame=frame;sampling=true;}
}
}
