#pragma once
#include <windows.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cwchar>
#include "runtime_log.h"
#include "perf_census.h"
#ifndef FFXV_PRESENT_TIMING_SECONDS
#define FFXV_PRESENT_TIMING_SECONDS 30
#endif
namespace PresentTiming {
enum Stage { XrTotal, XrWait, XrBegin, HudTotal, EyeTotal, KeyedWait, SwapWait, XrEnd, Desktop, Count };
struct Row { long long start{}, total{}, gap{}, period{}; unsigned long long swap{}; unsigned thread{},mode{},sync{},flags{}; long long ticks[Count]{}; unsigned calls[Count]{}; };
inline SRWLOCK lock=SRWLOCK_INIT;
inline std::array<Row,8192> rows{};
inline unsigned count{};
inline std::atomic_bool active{},finished{};
inline long long deadline{};
inline ULONGLONG nextPoll{};
inline wchar_t trigger[MAX_PATH]{},output[MAX_PATH]{};
inline thread_local Row* current{};
inline thread_local long long previousExit{};
inline long long Now(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
inline long long Frequency(){static const auto f=[](){LARGE_INTEGER q{};QueryPerformanceFrequency(&q);return q.QuadPart;}();return f;}
inline void InitPaths(){
 if(trigger[0])return;
 wchar_t p[MAX_PATH]{};const auto n=GetModuleFileNameW(nullptr,p,MAX_PATH);if(!n||n>=MAX_PATH)return;
 auto* slash=wcsrchr(p,L'\\');if(!slash)return;slash[1]=0;
 swprintf_s(trigger,L"%sffxv_present_timing.start",p);
 swprintf_s(output,L"%sffxv_present_timing_%lu.csv",p,GetCurrentProcessId());
}
inline void Poll(){
 if(active.load(std::memory_order_relaxed)||finished.load(std::memory_order_relaxed))return;
 const auto now=GetTickCount64();
 AcquireSRWLockExclusive(&lock);
 if(!active && !finished && now>=nextPoll){
  nextPoll=now+1000;InitPaths();
  if(trigger[0] && GetFileAttributesW(trigger)!=INVALID_FILE_ATTRIBUTES && DeleteFileW(trigger)){
   count=0;deadline=Now()+Frequency()*FFXV_PRESENT_TIMING_SECONDS;active=true;
   RetailLogLine("[PRESENTTIMING] started bounded CPU timing window; no GPU readbacks");
  }
 }
 ReleaseSRWLockExclusive(&lock);
}
inline void Flush(){ // Called once with lock held, after measurement ends.
 FILE* file{};
 if(_wfopen_s(&file,output,L"wb")||!file){RetailLogLine("[PRESENTTIMING] output open failed; capture stopped");return;}
 fputs("row,swapchain,thread,mode,sync,flags,start_qpc,total_ms,gap_ms,runtime_period_ms,xr_total_ms,xr_wait_ms,xr_begin_ms,hud_total_ms,eye_total_ms,keyed_wait_ms,swap_wait_ms,xr_end_ms,desktop_ms,xr_calls,hud_calls,eye_calls,keyed_calls,swap_calls\n",file);
 const double ms=1000.0/Frequency();
 for(unsigned i=0;i<count;++i){const auto& r=rows[i];
  fprintf(file,"%u,%llx,%u,%u,%u,%u,%lld,%.6f,%.6f,%.6f",i,r.swap,r.thread,r.mode,r.sync,r.flags,r.start,r.total*ms,r.gap*ms,r.period/1e6);
  for(auto ticks:r.ticks)fprintf(file,",%.6f",ticks*ms);
  fprintf(file,",%u,%u,%u,%u,%u\n",r.calls[XrTotal],r.calls[HudTotal],r.calls[EyeTotal],r.calls[KeyedWait],r.calls[SwapWait]);
 }
 fclose(file);RetailLogLine("[PRESENTTIMING] CSV saved beside game executable; capture and trigger polling stopped");
}
struct Frame {
 Row row{};bool recording{};bool root{};
 Frame(void* swap,unsigned sync,unsigned flags,unsigned mode){
  if(current || (flags&1))return; // nested Present / DXGI_PRESENT_TEST
  root=true;Poll();if(!active.load(std::memory_order_acquire))return;
  row.start=Now();row.swap=reinterpret_cast<unsigned long long>(swap);row.thread=GetCurrentThreadId();row.mode=mode;row.sync=sync;row.flags=flags;
  row.gap=previousExit && row.start>=previousExit?row.start-previousExit:0;
  recording=true;current=&row;
 }
 ~Frame(){
  if(!root)return;
  if(recording){
   const auto end=Now();row.total=end-row.start;current=nullptr;
   AcquireSRWLockExclusive(&lock);
   if(active){if(count<rows.size())rows[count++]=row;
    if(end>=deadline||count==rows.size()){active=false;finished=true;Flush();}}
   ReleaseSRWLockExclusive(&lock);
  }
  // Exclude the one-time CSV write from the next engine-gap measurement.
  if(!finished.load(std::memory_order_relaxed))previousExit=Now();
 }
};
struct Scope { Row* row; Stage stage;long long start;
 explicit Scope(Stage s):row(current),stage(s),start(Now()){}
 ~Scope(){const auto d=Now()-start;if(row){row->ticks[stage]+=d;++row->calls[stage];}
  PerfCensus::stageTicks[stage].fetch_add(d,std::memory_order_relaxed);PerfCensus::stageCalls[stage].fetch_add(1,std::memory_order_relaxed);}
};
template<class F> decltype(auto) Measure(Stage s,F&& f){Scope timer(s);return f();}
inline void Period(long long ns){if(current)current->period=ns;}
}
