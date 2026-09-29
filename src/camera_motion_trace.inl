// Passive camera-job trace. Published under a try-lock for an external reader.
// No camera writes, behavior switches, logging, allocation, or GPU work here.
struct CameraMotionRecord
{
    std::uint64_t job{}, presentBegin{}, presentEnd{}, camera{}, headSource{};
    std::uint64_t tickBegin{}, tickEnd{}, headSamples[4]{};
    std::uint32_t thread{}, output{}, readMask{}, flagsBefore{}, flagsAfter{}, kindOrder{};
    // 0: camera+500 begin; 1: head begin; 2/3: kind0 source/head;
    // 4/5: kind1 source/head; 6: camera+500 end; 7: head end.
    float matrices[8][16]{};
};
static_assert(sizeof(CameraMotionRecord) == 624);
struct CameraMotionSnapshot { SRWLOCK lock = SRWLOCK_INIT; CameraMotionRecord record{}; };
CameraMotionSnapshot g_cameraMotionLatest{};
thread_local CameraMotionRecord g_cameraMotionRecord{};
thread_local bool g_cameraMotionActive{};

void CaptureCameraMotionMatrix(unsigned index, const void* source)
{
    if (!source || index >= 8) return;
    __try
    {
        std::memcpy(g_cameraMotionRecord.matrices[index], source, 64);
        g_cameraMotionRecord.readMask |= 1u << index;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
std::uint32_t ReadCameraMotionFlags(const void* camera)
{
    __try { return *reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(camera) + 0x4F4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}
void BeginCameraMotionTrace(void* camera)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    g_cameraMotionActive = camera && g_stereoEnabled.load(std::memory_order_relaxed);
    if (!g_cameraMotionActive) return;
    auto& r = g_cameraMotionRecord;
    r = {};
    r.job = g_sepProbeActiveJobSequence;
    r.presentBegin = g_presentCount.load(std::memory_order_relaxed);
    r.camera = reinterpret_cast<std::uintptr_t>(camera);
    r.headSource = g_dynamicHeadSource.load(std::memory_order_acquire);
    r.tickBegin = GetTickCount64();
    r.thread = GetCurrentThreadId();
    r.output = ReadTrueOutput();
    r.flagsBefore = ReadCameraMotionFlags(camera);
    r.headSamples[0] = g_splitLeftSamples.load(std::memory_order_acquire);
    CaptureCameraMotionMatrix(0, static_cast<const std::uint8_t*>(camera) + 0x500);
    CaptureCameraMotionMatrix(1, reinterpret_cast<const void*>(r.headSource));
}
void CaptureCameraMotionBuilder(std::size_t kind, const float* source)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_cameraMotionActive || kind > 1 || !source) return;
    auto& r = g_cameraMotionRecord;
    r.kindOrder = (r.kindOrder << 4) | static_cast<unsigned>(kind + 1);
    r.headSamples[kind + 1] = g_splitLeftSamples.load(std::memory_order_acquire);
    CaptureCameraMotionMatrix(2 + static_cast<unsigned>(kind) * 2,
        kind == 0 ? source : source - 16);
    CaptureCameraMotionMatrix(3 + static_cast<unsigned>(kind) * 2,
        reinterpret_cast<const void*>(r.headSource));
}
void EndCameraMotionTrace()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_cameraMotionActive) return;
    auto& r = g_cameraMotionRecord;
    CaptureCameraMotionMatrix(6, reinterpret_cast<const void*>(r.camera + 0x500));
    CaptureCameraMotionMatrix(7, reinterpret_cast<const void*>(r.headSource));
    r.flagsAfter = ReadCameraMotionFlags(reinterpret_cast<const void*>(r.camera));
    r.headSamples[3] = g_splitLeftSamples.load(std::memory_order_acquire);
    r.presentEnd = g_presentCount.load(std::memory_order_relaxed);
    r.tickEnd = GetTickCount64();
    if (TryAcquireSRWLockExclusive(&g_cameraMotionLatest.lock))
    {
        g_cameraMotionLatest.record = r;
        ReleaseSRWLockExclusive(&g_cameraMotionLatest.lock);
    }
    g_cameraMotionActive = false;
}

// Capture the validated source that actually receives the right-eye offset,
// including native builder calls outside RetailCameraJobPolicy.
CameraMotionSnapshot g_cameraValidSourceLatest{};
std::atomic_uint64_t g_cameraValidSourceSequence{};
void CaptureValidRightCameraSource(const float* transform)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    CameraMotionRecord r{};
    r.job = g_cameraValidSourceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    r.presentBegin = g_presentCount.load(std::memory_order_relaxed);
    r.camera = reinterpret_cast<std::uintptr_t>(transform); // input address, not owner
    r.headSource = g_dynamicHeadSource.load(std::memory_order_acquire);
    r.tickBegin = GetTickCount64();
    r.thread = GetCurrentThreadId();
    r.output = ReadTrueOutput();
    r.kindOrder = g_sepProbeActiveJobSequence ? 1u : 0u; // wrapped versus native
    r.flagsBefore = static_cast<std::uint32_t>(g_dynamicHeadPhase.load(std::memory_order_acquire));
    r.headSamples[0] = g_splitLeftSamples.load(std::memory_order_acquire);
    __try
    {
        std::memcpy(r.matrices[2], transform, 64);
        r.readMask |= 4u;
        if (r.headSource)
        {
            std::memcpy(r.matrices[3], reinterpret_cast<const void*>(r.headSource), 64);
            r.readMask |= 8u;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    r.headSamples[3] = g_splitLeftSamples.load(std::memory_order_acquire);
    r.presentEnd = g_presentCount.load(std::memory_order_relaxed);
    r.tickEnd = GetTickCount64();
    if (TryAcquireSRWLockExclusive(&g_cameraValidSourceLatest.lock))
    {
        g_cameraValidSourceLatest.record = r;
        ReleaseSRWLockExclusive(&g_cameraValidSourceLatest.lock);
    }
}
