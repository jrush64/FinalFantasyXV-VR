#pragma once
#include <d3d11.h>
#include <cstdint>

namespace UiMarkerAssets
{
enum class Kind : std::uint32_t { None, Target, Objective, SharedIcons, Interaction, Parking, CommonInteraction, FocusInfo, NpcTalk };
using LogFn = void (*)(const char*, ...);
inline const char* Name(Kind kind)
{
    return kind == Kind::NpcTalk ? "npc-talk" : kind == Kind::FocusInfo ? "focus-info" : kind == Kind::Target ? "target-cursor" :
        kind == Kind::Objective ? "destination-marker" :
        kind == Kind::SharedIcons ? "shared-icons" :
        kind == Kind::CommonInteraction ? "common-interaction" : kind == Kind::Interaction ? "interaction" : kind == Kind::Parking ? "parking-interaction" : "unmatched";
}

// FNV-1a of the base-mip BC3 bytes in the game's unmodified menu assets:
// menu/target_cursor/swf/swf.btex and menu/destination_marker/swf/swf.btex.
// Dimensions only bound readback work. They NEVER establish a match.
inline Kind Match(unsigned width, unsigned height, std::uint64_t hash)
{
    if (width == 240 && height == 176 && hash == 0xBF6F87EB980F8FE7ull) return Kind::Target;
    if (width == 320 && height == 112 && hash == 0xCEB03B6169A5D8A2ull) return Kind::Objective;
    if (width == 688 && height == 704 && hash == 0xF7AF52F29B2040F7ull) return Kind::SharedIcons;
    if (width == 272 && height == 128 && hash == 0x7CA396E171FCB8E3ull) return Kind::Interaction;
    if (width == 256 && height == 192 && hash == 0x90E9E5FC8C568EE9ull) return Kind::Parking;
    if(width==848 && height==768 && hash==0x1606B4AA5FB39E8Eull)return Kind::CommonInteraction;
    if(width==704 && height==528 && hash==0x847E2FCB7E42B3EBull)return Kind::FocusInfo;
    if(width==176 && height==208 && hash==0x0C79BDD83849A2B8ull)return Kind::NpcTalk;
    return Kind::None;
}
inline bool Candidate(const D3D11_TEXTURE2D_DESC& d)
{
    return ((d.Width == 240 && d.Height == 176) || (d.Width == 320 && d.Height == 112) ||
        (d.Width == 688 && d.Height == 704) || (d.Width == 272 && d.Height == 128) ||
        (d.Width == 256 && d.Height == 192) || (d.Width==848 && d.Height==768) || (d.Width==704 && d.Height==528) || (d.Width==176 && d.Height==208)) &&
        d.ArraySize == 1 && d.SampleDesc.Count == 1 &&
        (d.Format == DXGI_FORMAT_BC3_TYPELESS || d.Format == DXGI_FORMAT_BC3_UNORM ||
            d.Format == DXGI_FORMAT_BC3_UNORM_SRGB);
}
inline std::uint64_t HashRows(const void* bytes, unsigned width, unsigned height, unsigned pitch)
{
    const unsigned rowBytes = ((width + 3) / 4) * 16;
    if (!bytes || pitch < rowBytes) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned row = 0; row < (height + 3) / 4; ++row)
        for (unsigned x = 0; x < rowBytes; ++x)
            hash = (hash ^ static_cast<const std::uint8_t*>(bytes)[row * pitch + x]) * 1099511628211ull;
    return hash;
}

// Cache on the COM resource itself, so pointer reuse cannot carry a verdict.
inline constexpr GUID Tag = {0x62d16851,0x90a2,0x49af,{0x9a,0x72,0x16,0x09,0x17,0x1d,0xbc,0x03}};
inline bool ReadTag(ID3D11Texture2D* texture, Kind& kind)
{
    std::uint32_t value{}; UINT bytes = sizeof(value);
    if (FAILED(texture->GetPrivateData(Tag, &bytes, &value)) || bytes != sizeof(value) || value < 1 || value > 9)
        return false;
    kind = static_cast<Kind>(value - 1);
    return true;
}
inline void WriteTag(ID3D11Texture2D* texture, Kind kind)
{
    const auto value = static_cast<std::uint32_t>(kind) + 1;
    texture->SetPrivateData(Tag, sizeof(value), &value);
}

// At most four small in-flight staging copies and 32 attempts per run. No
// Flush, event wait, or blocking Map in the game. Publication happens only
// at Present, so all copies of a sprite in one frame see the same verdict.
struct Tracker
{
    struct Guard
    {
        SRWLOCK* lock;
        explicit Guard(SRWLOCK* value) : lock(value) { AcquireSRWLockExclusive(lock); }
        ~Guard() { ReleaseSRWLockExclusive(lock); }
    };
    SRWLOCK lock = SRWLOCK_INIT;
    struct Pending
    {
        ID3D11Texture2D* source{};
        ID3D11Texture2D* staging{};
        unsigned width{}, height{}, frame{};
    } pending[4]{};
    unsigned attempts{}, matches{}, mismatches{}, timeouts{};
    struct Counters { unsigned attempts, matches, mismatches, timeouts; };
    Counters Stats()
    {
        Guard guard(&lock);
        return {attempts,matches,mismatches,timeouts};
    }

    Kind Observe(ID3D11DeviceContext*, ID3D11Texture2D* texture, unsigned frame)
    {
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        if (!Candidate(desc)) return Kind::None;
        Guard guard(&lock);
        Kind kind{};
        if (ReadTag(texture, kind)) return kind;
        if (attempts >= 32) return Kind::None;
        for (auto& p : pending) if (p.source == texture) return Kind::None;
        for (auto& p : pending)
        {
            if (p.source) continue;
            ++attempts;
            texture->AddRef();
            p = {texture, nullptr, desc.Width, desc.Height, frame};
            break;
        }
        return Kind::None;
    }

    void Poll(ID3D11DeviceContext* context, unsigned frame, LogFn log)
    {
        Guard guard(&lock);
        for (auto& p : pending)
        {
            if (!p.source) continue;
            if (!p.staging)
            {
                // A marker may first be seen on a deferred draw context.
                // Schedule its copy on the immediate context at Present,
                // after game command-list execution, then poll next Present.
                D3D11_TEXTURE2D_DESC d{}; p.source->GetDesc(&d);
                d.MipLevels=1; d.Usage=D3D11_USAGE_STAGING;
                d.BindFlags=d.MiscFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ID3D11Device* device{}; p.source->GetDevice(&device);
                const auto created=device->CreateTexture2D(&d,nullptr,&p.staging);
                device->Release();
                if (FAILED(created))
                {
                    ++timeouts;
                    if (log) log("[UIMARK] staging creation failed hr=%08lX", static_cast<unsigned long>(created));
                    p.source->Release(); p={};
                    continue;
                }
                context->CopySubresourceRegion(p.staging,0,0,0,0,p.source,0,nullptr);
                p.frame=frame;
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE data{};
            const auto hr = context->Map(p.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING && frame - p.frame < 120) continue;
            if (SUCCEEDED(hr))
            {
                const auto hash = HashRows(data.pData, p.width, p.height, data.RowPitch);
                context->Unmap(p.staging, 0);
                const auto kind = Match(p.width, p.height, hash);
                WriteTag(p.source, kind);
                if (kind == Kind::None) ++mismatches; else ++matches;
                if (log) log("[UIMARK] asset=%s tex=%ux%u resource=%p hash=%016llX",
                    Name(kind), p.width, p.height, static_cast<void*>(p.source),
                    static_cast<unsigned long long>(hash));
            }
            else
            {
                ++timeouts;
                if (log) log("[UIMARK] readback unavailable tex=%ux%u hr=%08lX", p.width, p.height,
                    static_cast<unsigned long>(hr));
            }
            p.source->Release(); p.staging->Release(); p = {};
        }
    }
};
}
