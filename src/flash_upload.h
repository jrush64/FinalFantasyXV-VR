#pragma once
#include "lightning_geometry_fix.h"
// Probe-only snapshots: never use the guard's tag or tracked-buffer set.
namespace FlashUpload {
inline const GUID Tag={0x61bd27f3,0x280d,0x492b,{0xb4,0x4f,0x37,0xa8,0x7e,0x49,0x60,0xd1}};
inline std::atomic<std::uintptr_t> tracked[256]{};
inline std::atomic_ullong armed{},copied{},copiedBytes{},oversize{},rangeMiss{};
inline unsigned Slot(std::uintptr_t k){return unsigned(((k>>4)^(k>>13))&255);}
inline bool Known(ID3D11Resource* r){if(!r)return false;auto k=reinterpret_cast<std::uintptr_t>(r);for(unsigned i=0;i<8;++i){auto v=tracked[(Slot(k)+i)&255].load(std::memory_order_relaxed);if(v==k)return true;if(!v)return false;}return false;}
inline void Track(ID3D11Resource* r){auto k=reinterpret_cast<std::uintptr_t>(r);for(unsigned i=0;i<8;++i){auto& v=tracked[(Slot(k)+i)&255];auto old=v.load();if(old==k)return;if(!old && v.compare_exchange_strong(old,k))return;}tracked[Slot(k)].store(k);}
inline LightningGeometryFix::Upload* Get(ID3D11Resource* r){if(!Known(r))return nullptr;LightningGeometryFix::Upload* p{};UINT n=sizeof(p);if(FAILED(r->GetPrivateData(Tag,&n,&p)))return nullptr;return p;}
inline void Arm(ID3D11Buffer* b){
 if(!b)return;auto* p=Get(b);if(p){p->Release();return;}
 p=new(std::nothrow) LightningGeometryFix::Upload;
 if(p){if(SUCCEEDED(b->SetPrivateDataInterface(Tag,p))){Track(b);++armed;}p->Release();}
}
inline void Invalidate(ID3D11Resource* r){if(auto* p=Get(r)){AcquireSRWLockExclusive(&p->lock);p->known=false;ReleaseSRWLockExclusive(&p->lock);p->Release();}}
inline void Updated(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,const void* data){
 auto* p=Get(r);if(!p)return;AcquireSRWLockExclusive(&p->lock);p->known=false;
 if(c && c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE && !sub && data){
  ID3D11Buffer* b{};if(SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&b)))){
   D3D11_BUFFER_DESC d{};b->GetDesc(&d);b->Release();const UINT first=box?box->left:0,last=box?box->right:d.ByteWidth;
   if(first<last && last<=d.ByteWidth){
    if(last-first<=512*1024){try{p->bytes.resize(last-first);memcpy(p->bytes.data(),data,last-first);p->offset=first;p->known=true;++copied;copiedBytes+=last-first;}catch(...){p->known=false;}}
    else ++oversize;
   }
  }
 }
 ReleaseSRWLockExclusive(&p->lock);p->Release();
}
inline bool Read(ID3D11Resource* r,UINT offset,void* out,UINT size){auto* p=Get(r);if(!p)return false;bool ok=false;AcquireSRWLockShared(&p->lock);
 if(p->known && offset>=p->offset && offset-p->offset<=p->bytes.size() && size<=p->bytes.size()-(offset-p->offset)){memcpy(out,p->bytes.data()+offset-p->offset,size);ok=true;}else ++rangeMiss;
 ReleaseSRWLockShared(&p->lock);p->Release();return ok;
}
}
