#pragma once
// Temporary, one-launch capture of real game world-to-canvas projections.
// No game data, draw state, camera, or function result is changed.
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "MinHook.h"
#include "research_diagnostics.h"
namespace UiWorldProbe {
using LogFn = void(*)(const char*, ...);
using ProjectFn = std::uintptr_t(__fastcall*)(std::uint32_t,const float*,float*,std::uint32_t);
struct Record {
    std::uint32_t kind, present, thread, detail;
    std::uint64_t ticks, id, caller;
    float input[4], output[4], extra[4];
};
static_assert(sizeof(Record)==88);
struct Header { char magic[8]; std::uint32_t version, recordBytes; std::uint64_t frequency; };
inline ProjectFn original{};
inline std::uintptr_t base{};
inline LogFn logger{};
inline HANDLE file=INVALID_HANDLE_VALUE;
inline SRWLOCK lock=SRWLOCK_INIT;
inline Record records[1024]{}, pending[1024]{};
inline unsigned count{}, windows{}, endPresent{}, nextPresent{};
inline std::atomic_bool markerSeen{};
inline bool waitingForStart{},denseCapture{},rolling{};
inline constexpr unsigned RingSlots=600;
inline std::uint64_t nextSampleMs{};
inline wchar_t ringFolder[MAX_PATH]{};
inline wchar_t startPath[MAX_PATH]{};
inline bool PollDeferredStart(unsigned present) {
    if(!waitingForStart)return true;
    if(present%30 || !startPath[0] || !DeleteFileW(startPath))return false;
    waitingForStart=false;denseCapture=true;nextPresent=present;markerSeen.store(true);
    if(logger)logger("[UIWORLDPROBE] live start consumed; capture begins now: 64 bounded windows, 30-present gaps");
    return true;
}
inline std::atomic_uint frame{}, generation{}, missed{}, overflow{};
inline std::atomic_bool installed{}, enabled{};
inline bool Read(void* out,const void* in,std::size_t n) {
    __try { std::memcpy(out,in,n); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void Append(Record record,unsigned stamp) {
    if (!stamp || generation.load(std::memory_order_acquire)!=stamp) return;
    if (!TryAcquireSRWLockExclusive(&lock)) { ++missed; return; }
    if (generation.load(std::memory_order_acquire)==stamp) {
        if(count<1024) records[count++]=record; else ++overflow;
    }
    ReleaseSRWLockExclusive(&lock);
}
inline Record NewRecord(unsigned kind,unsigned detail) {
    Record record{}; record.kind=kind; record.detail=detail;
    record.present=frame.load(std::memory_order_relaxed); record.thread=GetCurrentThreadId();
    LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); record.ticks=tick.QuadPart;
    return record;
}
inline std::uintptr_t Invoke(std::uint32_t scene,const float* point,float* screen,std::uint32_t camera,std::uintptr_t caller) {
    const unsigned stamp=generation.load(std::memory_order_acquire);
    Record record{};
    bool readable=false;
    if(stamp) {
        record=NewRecord(1,scene); record.id=reinterpret_cast<std::uintptr_t>(screen);
        record.caller=caller-base;
        record.extra[0]=static_cast<float>(camera);
        readable=point && Read(record.input,point,sizeof(record.input));
    }
    const auto result=original(scene,point,screen,camera);
    if(readable && screen && Read(record.output,screen,sizeof(record.output))) {
        record.extra[1]=static_cast<float>(result & 0xff);
        Append(record,stamp);
    }
    return result;
}
inline std::uintptr_t __fastcall Hook(std::uint32_t scene,const float* point,float* screen,std::uint32_t camera) {
    return Invoke(scene,point,screen,camera,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
inline bool Sampling() { if constexpr (!ResearchDiagnostics::MarkerCapture) return false; return generation.load(std::memory_order_acquire)!=0; }
inline void SeenMarker() { if(enabled.load(std::memory_order_relaxed)) markerSeen.store(true,std::memory_order_relaxed); }
inline void Sprite(unsigned kind,unsigned copy,unsigned ordinal,const float* root,const float* bounds,const float* uvRoot) {
    if constexpr (!ResearchDiagnostics::MarkerCapture) return;
    const auto stamp=generation.load(std::memory_order_acquire);
    if(!stamp) return;
    auto record=NewRecord(2,kind); record.id=copy; record.caller=ordinal;
    std::memcpy(record.input,root,16); std::memcpy(record.output,bounds,16); std::memcpy(record.extra,uvRoot,16);
    Append(record,stamp);
}
inline bool WriteAll(const void* data,DWORD bytes) {
    DWORD written{};
    return file!=INVALID_HANDLE_VALUE && WriteFile(file,data,bytes,&written,nullptr) && written==bytes;
}
// One snapshot per second, retaining ten minutes regardless of frame rate.
// Rotating files bound disk use without ever ending the recording session.
// Publish by rename so readers only see complete header+record files.
inline bool WriteRing(const wchar_t* stream,unsigned sequence,const void* data,DWORD bytes,
                      unsigned recordBytes,const char* magic) {
    wchar_t path[MAX_PATH]{},temporary[MAX_PATH]{};
    if(swprintf_s(path,L"%s\\%s_%03u.bin",ringFolder,stream,sequence%RingSlots)<0 ||
       swprintf_s(temporary,L"%s.tmp",path)<0)return false;
    HANDLE out=CreateFileW(temporary,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(out==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    Header header{};std::memcpy(header.magic,magic,8);header.version=1;
    header.recordBytes=recordBytes;header.frequency=static_cast<std::uint64_t>(frequency.QuadPart);
    DWORD n{};
    const bool ok=WriteFile(out,&header,sizeof(header),&n,nullptr) && n==sizeof(header) &&
        WriteFile(out,data,bytes,&n,nullptr) && n==bytes;
    CloseHandle(out);
    if(!ok)return false;
    return MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING)!=0;
}
inline void Present(unsigned present,std::uint64_t nowMs=GetTickCount64()) {
    if constexpr (!ResearchDiagnostics::MarkerCapture) return;
    frame.store(present,std::memory_order_relaxed);
    if(!enabled.load(std::memory_order_acquire)) return;
    if(!PollDeferredStart(present))return;
    if(Sampling() && present>=endPresent) {
        generation.store(0,std::memory_order_release);
        AcquireSRWLockExclusive(&lock);
        const unsigned n=count; std::memcpy(pending,records,n*sizeof(Record)); count=0;
        ReleaseSRWLockExclusive(&lock);
        const bool ok=rolling ? WriteRing(L"world",windows,pending,n*sizeof(Record),sizeof(Record),"FFXVWP01") : WriteAll(pending,n*sizeof(Record));
        const auto lost=missed.exchange(0), full=overflow.exchange(0);
        if(logger) logger("[UIWORLDPROBE] window=%u records=%u contentionDrops=%u overflow=%u write=%s",windows,n,lost,full,ok?"ok":"failed");
        if(!ok || (!rolling && windows>=(denseCapture?64u:16u))) {
            enabled.store(false,std::memory_order_release);
            CloseHandle(file); file=INVALID_HANDLE_VALUE;
            if(logger) logger("[UIWORLDPROBE] capture stopped; visual routing unchanged; next launch is inactive without a new .once file");
            return;
        }
        nextPresent=present+(denseCapture?30:240);
    }
    if(!Sampling() && (rolling ? nowMs>=nextSampleMs :
        present>=nextPresent && markerSeen.exchange(false,std::memory_order_relaxed))) {
        if(rolling)nextSampleMs=nowMs+1000;
        endPresent=present+8; ++windows;
        generation.store(windows,std::memory_order_release);
    }
}
inline bool VerifyImage(std::uintptr_t module) {
    constexpr unsigned char expected[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x57,0x48,0x81,0xec,0x30,0x01,0x00,0x00,0x0f,0x28,0x02,0x8b,0xf1,0x41,0x0f,0x29,0x00,0x41,0x8b,0xf9,0x48};
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{}; unsigned char actual[sizeof(expected)]{};
    if(!module || !Read(&dos,reinterpret_cast<void*>(module),sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000) return false;
    if(!Read(&nt,reinterpret_cast<void*>(module+dos.e_lfanew),sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.TimeDateStamp!=0x5f85274e || nt.OptionalHeader.SizeOfImage!=0xfecb000) return false;
    return Read(actual,reinterpret_cast<void*>(module+0x564220),sizeof(actual)) && std::memcmp(actual,expected,sizeof(actual))==0;
}
inline void Install(LogFn log,bool externalProvider=false) {
    if constexpr (!ResearchDiagnostics::MarkerCapture) return;
    if(installed.exchange(true)) return;
    logger=log;
    wchar_t executable[MAX_PATH]{};
    const auto length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
    if(!length || length>=MAX_PATH) return;
    auto slash=wcsrchr(executable,L'\\'); if(!slash) return;
    if(_wcsicmp(slash+1,L"ffxv_s.exe")!=0) return;
    slash[1]=0;
    wchar_t once[MAX_PATH]{}, output[MAX_PATH]{};
    if(swprintf_s(once,L"%sffxv_world_marker_capture.once",executable)<0) return;
    if(GetFileAttributesW(once)==INVALID_FILE_ATTRIBUTES) return;
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(!externalProvider && !VerifyImage(base)) { if(logger) logger("[UIWORLDPROBE] refused: executable version or projection entry mismatch"); return; }
    SYSTEMTIME time{}; GetLocalTime(&time);
    if(swprintf_s(output,L"%sffxv_world_marker_capture_%04u%02u%02u_%02u%02u%02u_%lu.bin",executable,time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId())<0) return;
    file=CreateFileW(output,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) { if(logger) logger("[UIWORLDPROBE] cannot open capture file error=%lu",GetLastError()); return; }
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    Header header{{'F','F','X','V','W','P','0','1'},1,sizeof(Record),static_cast<std::uint64_t>(frequency.QuadPart)};
    if(!WriteAll(&header,sizeof(header))) { CloseHandle(file); file=INVALID_HANDLE_VALUE; return; }
    auto target=reinterpret_cast<void*>(base+0x564220);
    const auto created=externalProvider ? MH_OK : MH_CreateHook(target,reinterpret_cast<void*>(&Hook),reinterpret_cast<void**>(&original));
    const auto status=externalProvider ? MH_OK : created==MH_OK ? MH_EnableHook(target) : created;
    if(status!=MH_OK) {
        if(created==MH_OK) MH_RemoveHook(target);
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if(logger) logger("[UIWORLDPROBE] hook failed: %s",MH_StatusToString(status));
        return;
    }
    // Consume only the explicit, one-launch request; ordinary play stays inactive.
    if(!DeleteFileW(once)) {
        if(!externalProvider) {MH_DisableHook(target); MH_RemoveHook(target);}
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if(logger) logger("[UIWORLDPROBE] stopped: cannot consume one-launch request");
        return;
    }
    wchar_t waitPath[MAX_PATH]{};
    swprintf_s(waitPath,L"%sffxv_ui_capture_wait.once",executable);
    swprintf_s(startPath,L"%sffxv_ui_capture.start",executable);
    waitingForStart=DeleteFileW(waitPath)!=0;
    wchar_t rollingFlag[MAX_PATH]{};
    swprintf_s(rollingFlag,L"%sffxv_ui_rolling.once",executable);
    if(GetFileAttributesW(rollingFlag)!=INVALID_FILE_ATTRIBUTES) {
        swprintf_s(ringFolder,L"%sffxv_capture_ring_%lu_%llu",executable,GetCurrentProcessId(),GetTickCount64());
        if(!CreateDirectoryW(ringFolder,nullptr) || !DeleteFileW(rollingFlag)) {
            if(logger)logger("[UIWORLDPROBE] ERROR: rolling capture setup failed; not silently using expiring mode");
            CloseHandle(file);file=INVALID_HANDLE_VALUE;return;
        }
        rolling=true;waitingForStart=false;CloseHandle(file);file=INVALID_HANDLE_VALUE;
        if(logger)logger("[UIWORLDPROBE] ROLLING capture active for entire session; 1 snapshot/second, 600 retained windows; no marker gate, no duration cutoff");
    }
    enabled.store(true,std::memory_order_release);
    if(waitingForStart && logger)logger("[UIWORLDPROBE] waiting for live ffxv_ui_capture.start; drive time does not consume capture windows");
    if(!rolling && logger) logger("[UIWORLDPROBE] armed one-launch read-only projection capture; 16 windows x 8 presents, 240-present gaps; rendering unchanged");
}
}
