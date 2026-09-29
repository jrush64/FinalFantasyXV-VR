// Passive CPU-stage motion trace. No engine state changes or new hooks.
// Stages: 1=head commit, 2/3=validated builder source before offset,
// 4/5=builder result before source restore, 6=bank selector before apply,
// 7/8=uploadMatrices entry/exit, 9=current getter, 10=actual writer destination.
// Stages 9/10 reserved bit0: primary pointer equals cached tracked camera.
// These are NOT captured GPU constants.
struct RenderMotionRecord
{
    std::uint64_t sequence{}, qpc{}, present{}, headBegin{}, source{}, renderer{}, bank0{}, bank1{}, headEnd{};
    std::uint32_t stage{}, output{}, thread{}, mask{}, phase{}, history{}, wrapped{}, reserved{};
    // primary, head, bank0 view, bank1 view, renderer current view/VP,
    // renderer indexed view, renderer other indexed view.
    float matrices[8][16]{};
};
static_assert(sizeof(RenderMotionRecord) == 616);
struct RenderMotionRing
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::uint64_t writes{};
    RenderMotionRecord records[256]{};
};
RenderMotionRing g_renderMotionRing{};
std::atomic_uint64_t g_renderMotionDropped{};
void CopyRenderMotionMatrix(RenderMotionRecord& r, unsigned cell, const void* source)
{
    if (!source || cell >= 8) return;
    __try
    {
        std::memcpy(r.matrices[cell], source, 64);
        r.mask |= 1u << cell;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void CaptureRenderMotion(unsigned stage, const float* primary)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_stereoEnabled.load(std::memory_order_relaxed)) return;
    RenderMotionRecord r{};
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    r.qpc = now.QuadPart;
    r.present = g_presentCount.load(std::memory_order_relaxed);
    r.headBegin = g_splitLeftSamples.load(std::memory_order_acquire);
    r.stage = stage; r.output = static_cast<unsigned>(ReadTrueOutput());
    r.thread = GetCurrentThreadId();
    r.phase = static_cast<unsigned>(g_dynamicHeadPhase.load(std::memory_order_acquire));
    r.wrapped = g_sepProbeActiveJobSequence ? 1u : 0u;
    r.source = reinterpret_cast<std::uintptr_t>(primary);
    if (stage == 9 || stage == 10)
        r.reserved = r.source == g_dynamicHeadSource.load(std::memory_order_acquire) ? 1u : 0u;
    CopyRenderMotionMatrix(r, 0, primary);
    CopyRenderMotionMatrix(r, 1, reinterpret_cast<const void*>(g_dynamicHeadSource.load(std::memory_order_acquire)));
    __try
    {
        auto* area = *reinterpret_cast<std::uint8_t**>(g_exeBase + 0x45DF038);
        if (area)
        {
            r.bank0 = *reinterpret_cast<std::uintptr_t*>(area + 0x300);
            r.bank1 = *reinterpret_cast<std::uintptr_t*>(area + 0x308);
        }
        r.renderer = *reinterpret_cast<std::uintptr_t*>(g_exeBase + 0x4F29430);
        if (r.renderer)
        {
            r.history = *reinterpret_cast<std::uint32_t*>(r.renderer + 0xB821EC);
            const auto index = (r.history - 1u) & 1u;
            CopyRenderMotionMatrix(r, 4, reinterpret_cast<void*>(r.renderer + 0xB804C0));
            CopyRenderMotionMatrix(r, 5, reinterpret_cast<void*>(r.renderer + 0xB80540));
            CopyRenderMotionMatrix(r, 6, reinterpret_cast<void*>(r.renderer + 0xB82200 + index * 64));
            CopyRenderMotionMatrix(r, 7, reinterpret_cast<void*>(r.renderer + 0xB82200 + (index ^ 1u) * 64));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    CopyRenderMotionMatrix(r, 2, reinterpret_cast<void*>(r.bank0));
    CopyRenderMotionMatrix(r, 3, reinterpret_cast<void*>(r.bank1));
    r.headEnd = g_splitLeftSamples.load(std::memory_order_acquire);
    // Try-lock never waits in the camera exception handler or engine jobs.
    if (!TryAcquireSRWLockExclusive(&g_renderMotionRing.lock))
    { g_renderMotionDropped.fetch_add(1, std::memory_order_relaxed); return; }
    r.sequence = ++g_renderMotionRing.writes;
    g_renderMotionRing.records[(r.sequence - 1) & 255u] = r;
    ReleaseSRWLockExclusive(&g_renderMotionRing.lock);
}
