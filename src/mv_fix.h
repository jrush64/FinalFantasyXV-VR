#pragma once
// [MVFIX] DLSS 4 smears vegetation and water reflections when the
// head moves; the engine's own TAA does not (it clamps history hard, DLSS
// trusts the motion vectors).  Measured: most main-camera
// view uploads carry Previous* matrices ~11-12 px away from the current view
// at 5 m with the head still, i.e. a previous camera offset by about half the
// eye separation: the engine's "previous" is not the eye's own previous view,
// so every static pixel of that eye gets a fake, depth-dependent motion.
// (The engine-output label read on the uploading threads flips, so the fix
// below does not rely on it.)
//
// This module only OBSERVES: it keeps the last upload of every view constant
// buffer (IView_Combined_cbView, 768 B: Projection @0, View @64, ViewProj
// @128, InvView @192, ViewPort @320, ViewPoint @336, PreviousView @352,
// PreviousProj @416, PreviousViewProj @480), flagged main-camera when it is a
// perspective main-lens view with a full-size viewport.  The DLSS module looks
// up the buffer bound in each eye family's geometry pass and rebuilds the
// camera part of that eye's motion from its real current and previous views.
// The engine's data is never modified.
#include <d3d11.h>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace MvFix
{
// DLSS camera-motion correction on/off (Insert menu, View tab).
inline std::atomic_bool g_dlssFix{true};   // v3: gated by its own self-check

inline bool IsViewBufferUncached(ID3D11Resource* r)
{
    D3D11_RESOURCE_DIMENSION type{};
    r->GetType(&type);
    if (type != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    return d.ByteWidth == 768 && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}
struct CacheEntry { std::atomic<std::uintptr_t> key{}; std::atomic<int> view{}; };
inline CacheEntry g_cache[1024];
inline bool IsViewBuffer(ID3D11Resource* r)
{
    if (!r) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(r);
    auto& e = g_cache[((key >> 4) ^ (key >> 14)) & 1023];
    if (e.key.load(std::memory_order_acquire) == key) return e.view.load(std::memory_order_relaxed) != 0;
    const bool v = IsViewBufferUncached(r);
    e.view.store(v ? 1 : 0, std::memory_order_relaxed);
    e.key.store(key, std::memory_order_release);
    return v;
}

inline bool IsMainCamera(const float* f)
{
    const float p00 = f[0], p11 = f[5], persp = f[11];
    int size[2]{};
    std::memcpy(size, reinterpret_cast<const char*>(f) + 328, sizeof(size));
    return std::fabs(persp) > 0.5f && p11 > 0.7f && p11 < 1.6f && p00 > 0.3f && p00 < 1.0f &&
        size[0] >= 1280 && size[1] >= 720;
}

struct Snap { std::uintptr_t key{}; bool main{}; float f[192]{}; };
inline Snap g_snaps[256];
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::atomic<std::uint64_t> g_uploads{}, g_mainUploads{};

inline void Remember(ID3D11Resource* r, const float* f)
{
    const auto key = reinterpret_cast<std::uintptr_t>(r);
    const bool main = IsMainCamera(f);
    g_uploads.fetch_add(1, std::memory_order_relaxed);
    if (main) g_mainUploads.fetch_add(1, std::memory_order_relaxed);
    const std::size_t start = static_cast<std::size_t>(((key >> 4) ^ (key >> 12)) & 255);
    AcquireSRWLockExclusive(&g_lock);
    Snap* home = &g_snaps[start];
    for (std::size_t probe = 0; probe < 8; ++probe)
    {
        Snap& s = g_snaps[(start + probe) & 255];
        if (s.key == key || s.key == 0) { home = &s; break; }
    }
    home->key = key;
    home->main = main;
    if (main) std::memcpy(home->f, f, sizeof(home->f));
    ReleaseSRWLockExclusive(&g_lock);
}

// Last upload of this buffer, if it was a main-camera view.
inline bool Lookup(ID3D11Resource* r, float* out)
{
    if (!r) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(r);
    const std::size_t start = static_cast<std::size_t>(((key >> 4) ^ (key >> 12)) & 255);
    bool ok = false;
    AcquireSRWLockShared(&g_lock);
    for (std::size_t probe = 0; probe < 8; ++probe)
    {
        const Snap& s = g_snaps[(start + probe) & 255];
        if (s.key != key) continue;
        if (s.main) { std::memcpy(out, s.f, sizeof(s.f)); ok = true; }
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

// Map path: remember the mapped pointer of a view buffer on this thread.
inline thread_local ID3D11Resource* t_resource{};
inline thread_local void* t_data{};
inline void OnMapped(ID3D11Resource* r, D3D11_MAP type, void* data)
{
    if (!data || type == D3D11_MAP_READ || !IsViewBuffer(r)) return;
    t_resource = r;
    t_data = data;
}
inline void OnUnmapping(ID3D11Resource* r)
{
    if (!t_data || r != t_resource) return;
    float local[192];
    std::memcpy(local, t_data, sizeof(local));
    Remember(r, local);
    t_resource = nullptr;
    t_data = nullptr;
}
inline void OnUpdate(ID3D11Resource* r, const D3D11_BOX* box, const void* data)
{
    if (!data || box || !IsViewBuffer(r)) return;
    Remember(r, static_cast<const float*>(data));
}
}
