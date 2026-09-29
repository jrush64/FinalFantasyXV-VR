#include "research_diagnostics.h"
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Opt-in, four-image maximum. Read the acquired XR image before releasing it,
// then poll a staging resource without flushing or waiting on the GPU.
namespace ObjectivePixelProbe {
using Logger=void (*)(const char*,...);
class Capture {
    ID3D11Texture2D* staging_{};
    ID3D11Query* event_{};
    bool checked_{},armed_{},pending_{},hadFallback_{};
    unsigned worldCount_{},attempts_{},polls_{};
    std::uint64_t lastWorld_{},frame_{};
    std::wstring directory_;
    void Release() {
        if(event_)event_->Release(); if(staging_)staging_->Release();
        event_=nullptr;staging_=nullptr;pending_=false;
    }
public:
    std::uint64_t queuedFrame{};
    ~Capture(){Release();}
    bool Enabled() {
        if constexpr (!ResearchDiagnostics::MarkerCapture) return false;
        if(!checked_) {
            checked_=true;wchar_t path[32768]{};
            const DWORD n=GetModuleFileNameW(nullptr,path,32768);
            if(!n || n>=32768)return false;
            directory_=path;const auto slash=directory_.find_last_of(L"\\/");
            if(slash==std::wstring::npos)return false;
            directory_.resize(slash+1);
            armed_=DeleteFileW((directory_+L"ffxv_objective_pixels.once").c_str())!=FALSE;
        }
        return armed_ && !pending_ && attempts_<4;
    }
    bool Wants(bool world,std::uint64_t frame) {
        if(!Enabled())return false;
        if(world) {
            if(worldCount_>=3 || (worldCount_ && frame-lastWorld_<180))return false;
            ++worldCount_;lastWorld_=frame;
        } else { if(hadFallback_)return false;hadFallback_=true; }
        ++attempts_;return true;
    }
    bool Queue(ID3D11DeviceContext* context,ID3D11Texture2D* image,std::uint64_t frame,Logger log) {
        Release();queuedFrame=frame;D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
        if(desc.Width!=512 || desc.Height!=512 || (desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS) ||
           desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1) {
            log("[OBJECTIVEPIXELS] unsupported image %ux%u format=%u",desc.Width,desc.Height,desc.Format);return false;
        }
        ID3D11Device* device{};context->GetDevice(&device);
        // XR requested FLOAT, but the runtime may expose its underlying TYPELESS resource.
        // A typed staging copy in the same format family preserves all 64 pixel bits.
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HRESULT hr=device->CreateTexture2D(&desc,nullptr,&staging_);
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
        if(SUCCEEDED(hr))hr=device->CreateQuery(&q,&event_);device->Release();
        if(FAILED(hr)){Release();log("[OBJECTIVEPIXELS] create failed hr=%08X",unsigned(hr));return false;}
        context->CopyResource(staging_,image);context->End(event_);
        frame_=queuedFrame=frame;polls_=0;pending_=true;return true;
    }
    bool Poll(ID3D11DeviceContext* context,Logger log) {
        if(!pending_)return false;
        if(++polls_>600){log("[OBJECTIVEPIXELS] readback expired frame=%llu",frame_);Release();return false;}
        BOOL ready{};const HRESULT hr=context->GetData(event_,&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr==S_FALSE || (hr==S_OK && !ready))return false;
        if(FAILED(hr)){log("[OBJECTIVEPIXELS] query failed hr=%08X",unsigned(hr));Release();return false;}
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT mapped=context->Map(staging_,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if(mapped==DXGI_ERROR_WAS_STILL_DRAWING)return false;
        if(FAILED(mapped)){log("[OBJECTIVEPIXELS] map failed hr=%08X",unsigned(mapped));Release();return false;}
        std::vector<unsigned char> bytes(512*512*8);
        unsigned alpha=0,rgb=0,minX=512,minY=512,maxX=0,maxY=0;
        for(unsigned y=0;y<512;++y) {
            const auto* row=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch;
            std::memcpy(bytes.data()+y*4096,row,4096);
            for(unsigned x=0;x<512;++x) {
                const auto* p=reinterpret_cast<const std::uint16_t*>(row+x*8);
                if((p[0]&0x7fff)||(p[1]&0x7fff)||(p[2]&0x7fff))++rgb;
                if(p[3]>0x1419 && p[3]<0x7c00) { // finite positive alpha > .001
                    ++alpha;if(x<minX)minX=x;if(y<minY)minY=y;if(x>maxX)maxX=x;if(y>maxY)maxY=y;
                }
            }
        }
        context->Unmap(staging_,0);Release();
        std::uint32_t header[37]{};
        header[0]=0x20534444;header[1]=124;header[2]=0x100f;header[3]=header[4]=512;
        header[5]=4096;header[7]=1;header[19]=32;header[20]=4;header[21]=0x30315844;
        header[27]=0x1000;header[32]=DXGI_FORMAT_R16G16B16A16_FLOAT;header[33]=3;header[35]=1;
        wchar_t name[128]{};swprintf_s(name,L"ffxv_objective_pixels_%lu_%llu.dds",GetCurrentProcessId(),frame_);
        FILE* file{};bool saved=false;
        if(_wfopen_s(&file,(directory_+name).c_str(),L"wb")==0 && file) {
            saved=fwrite(header,1,sizeof(header),file)==sizeof(header) && fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
            saved=fclose(file)==0 && saved;
        }
        log("[OBJECTIVEPIXELS] frame=%llu alphaPixels=%u rgbPixels=%u bbox=%u/%u/%u/%u saved=%u file=ffxv_objective_pixels_%lu_%llu.dds",
            frame_,alpha,rgb,minX,minY,maxX,maxY,saved?1:0,GetCurrentProcessId(),frame_);
        return saved;
    }
};
}
