#pragma once
#include <windows.h>
#include <intrin.h>
#include <smmintrin.h>
#include <cstdint>
#include <cstring>
#include "perf_census.h"
namespace MappedCopy {
inline bool Supported(){static const bool value=[](){int c[4]{};__cpuid(c,1);return (c[2]&(1<<19))!=0;}();return value;}
// Mapping addresses are repeatedly reused. Cache memory-type hints per thread;
// recheck periodically. A stale hint can only affect speed: both copy paths
// are byte-exact for cached and write-combined memory.
inline bool WriteCombined(const void* source, std::size_t size){
 struct Hint {std::uintptr_t base{},end{};unsigned uses{};bool wc{};};
 static thread_local Hint hints[16]{};
 static thread_local unsigned recent{},next{};
 const auto address=reinterpret_cast<std::uintptr_t>(source);
 auto hit=[&](Hint& h){return h.uses && address>=h.base && address<h.end && size<=h.end-address;};
 if(hit(hints[recent])){--hints[recent].uses;return hints[recent].wc;}
 for(unsigned i=0;i<16;++i)if(hit(hints[i])){recent=i;--hints[i].uses;return hints[i].wc;}
 MEMORY_BASIC_INFORMATION info{};
 if(!VirtualQuery(source,&info,sizeof(info)))return false;
 recent=next++%16;
 auto& h=hints[recent];h={reinterpret_cast<std::uintptr_t>(info.BaseAddress),reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize,4096,(info.Protect&PAGE_WRITECOMBINE)!=0};
 return h.wc && address>=h.base && address<h.end && size<=h.end-address;
}
// Only used for a complete, still-mapped CPU upload before the real Unmap.
// Preserve all bytes and never read outside the supplied range. Ordinary CPU
// uploads and shadow-to-shadow transfers retain memcpy.
inline void Copy(void* destination,const void* source,std::size_t size){
 if(size<64 || !Supported() || !WriteCombined(source,size)){std::memcpy(destination,source,size);return;}
 PerfCensus::Add(PerfCensus::StoreWc);
 auto* d=static_cast<unsigned char*>(destination);auto* s=static_cast<const unsigned char*>(source);
 _mm_mfence();
 while(size && (reinterpret_cast<std::uintptr_t>(s)&15)){*d++=*s++;--size;}
 while(size>=64){
  const auto a=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(s)));
  const auto b=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(s+16)));
  const auto c=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(s+32)));
  const auto e=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(s+48)));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(d),a);_mm_storeu_si128(reinterpret_cast<__m128i*>(d+16),b);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(d+32),c);_mm_storeu_si128(reinterpret_cast<__m128i*>(d+48),e);
  s+=64;d+=64;size-=64;
 }
 while(size>=16){const auto a=_mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<unsigned char*>(s)));_mm_storeu_si128(reinterpret_cast<__m128i*>(d),a);s+=16;d+=16;size-=16;}
 while(size--)*d++=*s++;
 _mm_mfence();
}
}
