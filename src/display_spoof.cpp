#include "display_spoof.h"
#include "runtime_log.h"

#include <dxgi1_6.h>
#include <strsafe.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
// ---------------------------------------------------------------- config
unsigned g_targetW = 0;   // 0 = spoof disabled
unsigned g_targetH = 0;
unsigned g_sourceW = 0;   // real size of the display the game uses
unsigned g_sourceH = 0;
unsigned g_primaryW = 0;  // real primary size (GetSystemMetrics snapshot)
unsigned g_primaryH = 0;
std::atomic_bool g_active{false};

bool SizeIsSource(unsigned w, unsigned h) noexcept
{
    return g_active.load(std::memory_order_relaxed) && w == g_sourceW && h == g_sourceH;
}

bool SizeIsTarget(unsigned w, unsigned h) noexcept
{
    return g_active.load(std::memory_order_relaxed) && w == g_targetW && h == g_targetH;
}

// ---------------------------------------------------------------- early log
// The proxy opens its log on the init thread; attach-time lines would be
// dropped.  Keep them here and flush once the file exists.
constexpr int kEarlyLines = 96;
constexpr int kEarlyLineChars = 240;
char g_early[kEarlyLines][kEarlyLineChars]{};
int g_earlyCount = 0;
bool g_logOpen = false;
SRWLOCK g_earlyLock = SRWLOCK_INIT;

void LogLine(const char* line)
{
    AcquireSRWLockExclusive(&g_earlyLock);
    if (g_logOpen)
    {
        ReleaseSRWLockExclusive(&g_earlyLock);
        RetailLogLine(line);
        return;
    }
    if (g_earlyCount < kEarlyLines)
        StringCchCopyA(g_early[g_earlyCount++], kEarlyLineChars, line);
    ReleaseSRWLockExclusive(&g_earlyLock);
}

void LogF(const char* format, ...)
{
    char line[kEarlyLineChars]{};
    va_list args;
    va_start(args, format);
    StringCchVPrintfA(line, _countof(line), format, args);
    va_end(args);
    LogLine(line);
}

// ---------------------------------------------------------------- ini
bool ParseSize(const wchar_t* text, unsigned* w, unsigned* h)
{
    if (!text || !w || !h)
        return false;
    wchar_t* end{};
    const unsigned long width = std::wcstoul(text, &end, 10);
    if (end == text || (*end != L'x' && *end != L'X'))
        return false;
    const wchar_t* heightText = end + 1;
    const unsigned long height = std::wcstoul(heightText, &end, 10);
    if (end == heightText)
        return false;
    if (width < 640 || height < 480 || width > 16384 || height > 16384)
        return false;
    *w = static_cast<unsigned>(width);
    *h = static_cast<unsigned>(height);
    return true;
}

void ReadConfig(HMODULE self)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return;
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash)
        return;
    constexpr wchar_t name[] = L"ffxv-vr.ini";
    if (static_cast<std::size_t>(slash + 1 - path) + _countof(name) > _countof(path))
        return;
    std::memcpy(slash + 1, name, sizeof(name));

    wchar_t text[64]{};
    GetPrivateProfileStringW(L"Display", L"BackbufferWH", L"", text, _countof(text), path);
    unsigned w{}, h{};
    if (ParseSize(text, &w, &h))
    {
        g_targetW = w;
        g_targetH = h;
    }
    text[0] = L'\0';
    GetPrivateProfileStringW(L"Display", L"SourceWH", L"", text, _countof(text), path);
    if (ParseSize(text, &w, &h))
    {
        g_sourceW = w;
        g_sourceH = h;
    }
}

// ---------------------------------------------------------------- real displays
// No callbacks (EnumDisplayMonitors) at attach time: walk the device list.
void SnapshotDisplays()
{
    g_primaryW = static_cast<unsigned>(GetSystemMetrics(SM_CXSCREEN));
    g_primaryH = static_cast<unsigned>(GetSystemMetrics(SM_CYSCREEN));
    unsigned bestArea = 0;
    unsigned bestW = 0, bestH = 0;
    for (DWORD index = 0; index < 16; ++index)
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
            break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            continue;
        char nameA[40]{};
        WideCharToMultiByte(CP_ACP, 0, device.DeviceName, -1, nameA, sizeof(nameA), nullptr, nullptr);
        LogF("[DISPQ] display %s %ux%u @%uHz primary=%d", nameA, mode.dmPelsWidth,
            mode.dmPelsHeight, mode.dmDisplayFrequency,
            (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? 1 : 0);
        const unsigned area = mode.dmPelsWidth * mode.dmPelsHeight;
        const bool primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        if (area > bestArea || (area == bestArea && primary))
        {
            bestArea = area;
            bestW = mode.dmPelsWidth;
            bestH = mode.dmPelsHeight;
        }
    }
    if (!g_sourceW || !g_sourceH)
    {
        g_sourceW = bestW;
        g_sourceH = bestH;
    }
}

// ---------------------------------------------------------------- hooks: user32 / gdi32
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using GetSystemMetricsFn = int(WINAPI*)(int);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetDeviceCapsFn = int(WINAPI*)(HDC, int);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

EnumDisplaySettingsWFn g_origEnumDisplaySettingsW{};
EnumDisplaySettingsAFn g_origEnumDisplaySettingsA{};
GetSystemMetricsFn g_origGetSystemMetrics{};
GetMonitorInfoWFn g_origGetMonitorInfoW{};
GetMonitorInfoAFn g_origGetMonitorInfoA{};
GetDeviceCapsFn g_origGetDeviceCaps{};
SetWindowPosFn g_origSetWindowPos{};

std::atomic<int> g_enumLogs{0};
std::atomic<int> g_metricLogs{0};
std::atomic<int> g_monitorLogs{0};
std::atomic<int> g_capsLogs{0};
std::atomic<int> g_windowLogs{0};
std::atomic<int> g_dxgiLogs{0};

// Does this device name currently show the source mode?  Null = primary.
bool DeviceIsSourceW(LPCWSTR device)
{
    if (!g_origEnumDisplaySettingsW)
        return false;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    return g_origEnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode) &&
        SizeIsSource(mode.dmPelsWidth, mode.dmPelsHeight);
}

bool DeviceIsSourceA(LPCSTR device)
{
    if (!g_origEnumDisplaySettingsA)
        return false;
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    return g_origEnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, &mode) &&
        SizeIsSource(mode.dmPelsWidth, mode.dmPelsHeight);
}

// Number of real modes for a device (walk the original list once per call;
// only reached when an enumeration has already run off the end).
DWORD RealModeCountW(LPCWSTR device)
{
    DWORD count = 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    while (count < 4096 && g_origEnumDisplaySettingsW(device, count, &mode))
        ++count;
    return count;
}

DWORD RealModeCountA(LPCSTR device)
{
    DWORD count = 0;
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    while (count < 4096 && g_origEnumDisplaySettingsA(device, count, &mode))
        ++count;
    return count;
}

void LogEnum(const char* api, DWORD modeNum, BOOL ok, unsigned w, unsigned h, unsigned hz,
    bool rewrote, bool injected)
{
    const int n = g_enumLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 40)
        LogF("[DISPQ] %s mode=%ld -> ok=%d %ux%u @%uHz spoofed=%d injected=%d", api,
            static_cast<long>(modeNum), ok ? 1 : 0, w, h, hz, rewrote ? 1 : 0,
            injected ? 1 : 0);
}

BOOL WINAPI EnumDisplaySettingsWHook(LPCWSTR device, DWORD modeNum, DEVMODEW* dm) noexcept
{
    BOOL ok = g_origEnumDisplaySettingsW ? g_origEnumDisplaySettingsW(device, modeNum, dm) : FALSE;
    bool rewrote = false;
    bool injected = false;
    if (dm && g_active.load(std::memory_order_relaxed))
    {
        const bool special = modeNum == ENUM_CURRENT_SETTINGS || modeNum == ENUM_REGISTRY_SETTINGS;
        if (ok && special && SizeIsSource(dm->dmPelsWidth, dm->dmPelsHeight))
        {
            dm->dmPelsWidth = g_targetW;
            dm->dmPelsHeight = g_targetH;
            rewrote = true;
        }
        else if (!ok && !special && DeviceIsSourceW(device) && modeNum == RealModeCountW(device))
        {
            // One extra mode at the end of the list: the current mode at the
            // target size.
            DEVMODEW current{};
            current.dmSize = sizeof(current);
            if (g_origEnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &current))
            {
                const WORD size = dm->dmSize ? dm->dmSize : static_cast<WORD>(sizeof(DEVMODEW));
                const WORD copy = size < sizeof(DEVMODEW) ? size : static_cast<WORD>(sizeof(DEVMODEW));
                std::memcpy(dm, &current, copy);
                dm->dmSize = size;
                dm->dmPelsWidth = g_targetW;
                dm->dmPelsHeight = g_targetH;
                dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
                ok = TRUE;
                injected = true;
            }
        }
    }
    if (modeNum == ENUM_CURRENT_SETTINGS || modeNum == ENUM_REGISTRY_SETTINGS || injected ||
        !ok || modeNum == 0)
        LogEnum("EnumDisplaySettingsW", modeNum, ok, ok && dm ? dm->dmPelsWidth : 0u,
            ok && dm ? dm->dmPelsHeight : 0u, ok && dm ? dm->dmDisplayFrequency : 0u, rewrote,
            injected);
    return ok;
}

BOOL WINAPI EnumDisplaySettingsAHook(LPCSTR device, DWORD modeNum, DEVMODEA* dm) noexcept
{
    BOOL ok = g_origEnumDisplaySettingsA ? g_origEnumDisplaySettingsA(device, modeNum, dm) : FALSE;
    bool rewrote = false;
    bool injected = false;
    if (dm && g_active.load(std::memory_order_relaxed))
    {
        const bool special = modeNum == ENUM_CURRENT_SETTINGS || modeNum == ENUM_REGISTRY_SETTINGS;
        if (ok && special && SizeIsSource(dm->dmPelsWidth, dm->dmPelsHeight))
        {
            dm->dmPelsWidth = g_targetW;
            dm->dmPelsHeight = g_targetH;
            rewrote = true;
        }
        else if (!ok && !special && DeviceIsSourceA(device) && modeNum == RealModeCountA(device))
        {
            DEVMODEA current{};
            current.dmSize = sizeof(current);
            if (g_origEnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, &current))
            {
                const WORD size = dm->dmSize ? dm->dmSize : static_cast<WORD>(sizeof(DEVMODEA));
                const WORD copy = size < sizeof(DEVMODEA) ? size : static_cast<WORD>(sizeof(DEVMODEA));
                std::memcpy(dm, &current, copy);
                dm->dmSize = size;
                dm->dmPelsWidth = g_targetW;
                dm->dmPelsHeight = g_targetH;
                dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
                ok = TRUE;
                injected = true;
            }
        }
    }
    if (modeNum == ENUM_CURRENT_SETTINGS || modeNum == ENUM_REGISTRY_SETTINGS || injected ||
        !ok || modeNum == 0)
        LogEnum("EnumDisplaySettingsA", modeNum, ok, ok && dm ? dm->dmPelsWidth : 0u,
            ok && dm ? dm->dmPelsHeight : 0u, ok && dm ? dm->dmDisplayFrequency : 0u, rewrote,
            injected);
    return ok;
}

int WINAPI GetSystemMetricsHook(int index) noexcept
{
    int value = g_origGetSystemMetrics ? g_origGetSystemMetrics(index) : 0;
    bool rewrote = false;
    // Only the primary-screen extents, and only when the primary IS the
    // game's display.  SM_CXVIRTUALSCREEN spans every monitor; left alone.
    if (SizeIsSource(g_primaryW, g_primaryH))
    {
        if ((index == SM_CXSCREEN || index == SM_CXFULLSCREEN) &&
            value == static_cast<int>(g_primaryW))
        {
            value = static_cast<int>(g_targetW);
            rewrote = true;
        }
        else if ((index == SM_CYSCREEN || index == SM_CYFULLSCREEN) &&
            value == static_cast<int>(g_primaryH))
        {
            value = static_cast<int>(g_targetH);
            rewrote = true;
        }
    }
    if (index == SM_CXSCREEN || index == SM_CYSCREEN || index == SM_CXFULLSCREEN ||
        index == SM_CYFULLSCREEN)
    {
        const int n = g_metricLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16)
            LogF("[DISPQ] GetSystemMetrics(%d) -> %d spoofed=%d", index, value, rewrote ? 1 : 0);
    }
    return value;
}

bool RewriteMonitorInfo(LPMONITORINFO mi)
{
    if (!mi)
        return false;
    const unsigned w = static_cast<unsigned>(mi->rcMonitor.right - mi->rcMonitor.left);
    const unsigned h = static_cast<unsigned>(mi->rcMonitor.bottom - mi->rcMonitor.top);
    if (!SizeIsSource(w, h))
        return false;
    mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_targetW);
    mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_targetH);
    // rcWork stays consistent with rcMonitor or window placement goes odd.
    mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_targetW);
    mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_targetH);
    return true;
}

void LogMonitorInfo(const char* api, BOOL ok, LPMONITORINFO mi, bool rewrote)
{
    const int n = g_monitorLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 24 && ok && mi)
        LogF("[DISPQ] %s -> rcMonitor=%ldx%ld at (%ld,%ld) primary=%d spoofed=%d", api,
            mi->rcMonitor.right - mi->rcMonitor.left, mi->rcMonitor.bottom - mi->rcMonitor.top,
            mi->rcMonitor.left, mi->rcMonitor.top, (mi->dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0,
            rewrote ? 1 : 0);
}

BOOL WINAPI GetMonitorInfoWHook(HMONITOR monitor, LPMONITORINFO mi) noexcept
{
    const BOOL ok = g_origGetMonitorInfoW ? g_origGetMonitorInfoW(monitor, mi) : FALSE;
    const bool rewrote = ok && RewriteMonitorInfo(mi);
    LogMonitorInfo("GetMonitorInfoW", ok, mi, rewrote);
    return ok;
}

BOOL WINAPI GetMonitorInfoAHook(HMONITOR monitor, LPMONITORINFO mi) noexcept
{
    const BOOL ok = g_origGetMonitorInfoA ? g_origGetMonitorInfoA(monitor, mi) : FALSE;
    const bool rewrote = ok && RewriteMonitorInfo(mi);
    LogMonitorInfo("GetMonitorInfoA", ok, mi, rewrote);
    return ok;
}

int WINAPI GetDeviceCapsHook(HDC dc, int index) noexcept
{
    int value = g_origGetDeviceCaps ? g_origGetDeviceCaps(dc, index) : 0;
    bool rewrote = false;
    const bool horizontal = index == HORZRES || index == DESKTOPHORZRES;
    const bool vertical = index == VERTRES || index == DESKTOPVERTRES;
    if ((horizontal || vertical) && g_active.load(std::memory_order_relaxed) && dc)
    {
        // Both axes of this DC must be the source display before either is
        // rewritten, so a bitmap DC that happens to be 1440 tall is left alone.
        const bool desktop = index == DESKTOPHORZRES || index == DESKTOPVERTRES;
        const int w = g_origGetDeviceCaps(dc, desktop ? DESKTOPHORZRES : HORZRES);
        const int h = g_origGetDeviceCaps(dc, desktop ? DESKTOPVERTRES : VERTRES);
        if (SizeIsSource(static_cast<unsigned>(w), static_cast<unsigned>(h)))
        {
            value = static_cast<int>(horizontal ? g_targetW : g_targetH);
            rewrote = true;
        }
    }
    if (horizontal || vertical)
    {
        const int n = g_capsLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16)
            LogF("[DISPQ] GetDeviceCaps(%d) -> %d spoofed=%d", index, value, rewrote ? 1 : 0);
    }
    return value;
}

// Observer only: did the window actually get the spoofed size?
BOOL WINAPI SetWindowPosHook(HWND window, HWND after, int x, int y, int cx, int cy,
    UINT flags) noexcept
{
    const BOOL ok = g_origSetWindowPos ? g_origSetWindowPos(window, after, x, y, cx, cy, flags) : FALSE;
    if (g_active.load(std::memory_order_relaxed) && !(flags & SWP_NOSIZE))
    {
        const int n = g_windowLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 12)
        {
            RECT client{};
            GetClientRect(window, &client);
            LogF("[DISPQ] SetWindowPos hwnd=%p pos=(%d,%d) size=%dx%d flags=0x%X ok=%d client=%ldx%ld",
                static_cast<void*>(window), x, y, cx, cy, flags, ok ? 1 : 0,
                client.right - client.left, client.bottom - client.top);
        }
    }
    return ok;
}

bool InstallHook(void* target, void* detour, void** original, const char* name)
{
    if (!target)
    {
        LogF("[DISPQ] %s not found; hook skipped", name);
        return false;
    }
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
    {
        LogF("[DISPQ] MH_CreateHook %s failed: %s", name, MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        LogF("[DISPQ] MH_EnableHook %s failed: %s", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- hooks: IDXGIOutput
using OutputGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);
using OutputGetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput6*, DXGI_OUTPUT_DESC1*);
using GetDisplayModeListFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*,
    DXGI_MODE_DESC*);
using GetDisplayModeList1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput1*, DXGI_FORMAT, UINT, UINT*,
    DXGI_MODE_DESC1*);
using FindClosestMatchingModeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, const DXGI_MODE_DESC*,
    DXGI_MODE_DESC*, IUnknown*);
using FindClosestMatchingMode1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput1*,
    const DXGI_MODE_DESC1*, DXGI_MODE_DESC1*, IUnknown*);

OutputGetDescFn g_origOutputGetDesc{};
OutputGetDesc1Fn g_origOutputGetDesc1{};
GetDisplayModeListFn g_origGetDisplayModeList{};
GetDisplayModeList1Fn g_origGetDisplayModeList1{};
FindClosestMatchingModeFn g_origFindClosestMatchingMode{};
FindClosestMatchingMode1Fn g_origFindClosestMatchingMode1{};
std::atomic_bool g_outputsHooked{false};

// This output shows the game's display if its desktop rect is the source
// size, or already the target size (GetDesc goes through the GetMonitorInfoW
// hook inside dxgi.dll, so it can come back rewritten).
bool OutputIsOurs(IDXGIOutput* output)
{
    if (!output || !g_origOutputGetDesc || !g_active.load(std::memory_order_relaxed))
        return false;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(g_origOutputGetDesc(output, &desc)))
        return false;
    const unsigned w = static_cast<unsigned>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
    const unsigned h = static_cast<unsigned>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    return SizeIsSource(w, h) || SizeIsTarget(w, h);
}

bool RewriteDesktopRect(RECT* rect)
{
    if (!rect)
        return false;
    const unsigned w = static_cast<unsigned>(rect->right - rect->left);
    const unsigned h = static_cast<unsigned>(rect->bottom - rect->top);
    if (!SizeIsSource(w, h))
        return false;
    rect->right = rect->left + static_cast<LONG>(g_targetW);
    rect->bottom = rect->top + static_cast<LONG>(g_targetH);
    return true;
}

HRESULT STDMETHODCALLTYPE OutputGetDescHook(IDXGIOutput* self, DXGI_OUTPUT_DESC* desc) noexcept
{
    const HRESULT hr = g_origOutputGetDesc ? g_origOutputGetDesc(self, desc) : E_FAIL;
    const bool rewrote = SUCCEEDED(hr) && desc && RewriteDesktopRect(&desc->DesktopCoordinates);
    const int n = g_dxgiLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 && SUCCEEDED(hr) && desc)
        LogF("[DISPQ] IDXGIOutput::GetDesc -> desktop=%ldx%ld at (%ld,%ld) spoofed=%d",
            desc->DesktopCoordinates.right - desc->DesktopCoordinates.left,
            desc->DesktopCoordinates.bottom - desc->DesktopCoordinates.top,
            desc->DesktopCoordinates.left, desc->DesktopCoordinates.top, rewrote ? 1 : 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE OutputGetDesc1Hook(IDXGIOutput6* self, DXGI_OUTPUT_DESC1* desc) noexcept
{
    const HRESULT hr = g_origOutputGetDesc1 ? g_origOutputGetDesc1(self, desc) : E_FAIL;
    const bool rewrote = SUCCEEDED(hr) && desc && RewriteDesktopRect(&desc->DesktopCoordinates);
    const int n = g_dxgiLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 && SUCCEEDED(hr) && desc)
        LogF("[DISPQ] IDXGIOutput6::GetDesc1 -> desktop=%ldx%ld at (%ld,%ld) spoofed=%d",
            desc->DesktopCoordinates.right - desc->DesktopCoordinates.left,
            desc->DesktopCoordinates.bottom - desc->DesktopCoordinates.top,
            desc->DesktopCoordinates.left, desc->DesktopCoordinates.top, rewrote ? 1 : 0);
    return hr;
}

template <typename ModeDesc, typename Fn, typename Self>
HRESULT AppendTargetMode(const char* api, Fn original, Self* self, DXGI_FORMAT format, UINT flags,
    UINT* numModes, ModeDesc* modes)
{
    if (!original)
        return E_FAIL;
    if (!numModes)
        return original(self, format, flags, numModes, modes);
    const bool ours = OutputIsOurs(self);
    bool appended = false;
    HRESULT hr;
    UINT count = *numModes;
    if (!modes)
    {
        // Count query: promise one more so the caller allocates room.
        hr = original(self, format, flags, &count, nullptr);
        if (SUCCEEDED(hr) && ours)
            ++count;
        *numModes = count;
    }
    else
    {
        UINT filled = count;
        hr = original(self, format, flags, &filled, modes);
        if (SUCCEEDED(hr) && ours && filled < count)
        {
            ModeDesc extra{};
            if (filled > 0)
                extra = modes[filled - 1];
            else
            {
                extra.Format = format;
                extra.RefreshRate.Numerator = 60;
                extra.RefreshRate.Denominator = 1;
            }
            extra.Width = g_targetW;
            extra.Height = g_targetH;
            modes[filled] = extra;
            ++filled;
            appended = true;
        }
        *numModes = filled;
    }
    const int n = g_dxgiLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 32)
        LogF("[DISPQ] %s fmt=%d flags=0x%X listFilled=%d hr=0x%08X count=%u ours=%d appended=%d",
            api, static_cast<int>(format), flags, modes ? 1 : 0, static_cast<unsigned>(hr),
            *numModes, ours ? 1 : 0, appended ? 1 : 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDisplayModeListHook(IDXGIOutput* self, DXGI_FORMAT format, UINT flags,
    UINT* numModes, DXGI_MODE_DESC* modes) noexcept
{
    return AppendTargetMode("IDXGIOutput::GetDisplayModeList", g_origGetDisplayModeList, self,
        format, flags, numModes, modes);
}

HRESULT STDMETHODCALLTYPE GetDisplayModeList1Hook(IDXGIOutput1* self, DXGI_FORMAT format,
    UINT flags, UINT* numModes, DXGI_MODE_DESC1* modes) noexcept
{
    return AppendTargetMode("IDXGIOutput1::GetDisplayModeList1", g_origGetDisplayModeList1, self,
        format, flags, numModes, modes);
}

template <typename ModeDesc, typename Fn, typename Self>
HRESULT ClosestTargetMode(const char* api, Fn original, Self* self, const ModeDesc* want,
    ModeDesc* got, IUnknown* device)
{
    if (!original)
        return E_FAIL;
    HRESULT hr = original(self, want, got, device);
    bool rewrote = false;
    if (want && got && OutputIsOurs(self) && SizeIsTarget(want->Width, want->Height))
    {
        if (FAILED(hr))
        {
            *got = *want;
            if (got->RefreshRate.Denominator == 0)
            {
                got->RefreshRate.Numerator = 60;
                got->RefreshRate.Denominator = 1;
            }
            hr = S_OK;
        }
        got->Width = g_targetW;
        got->Height = g_targetH;
        rewrote = true;
    }
    const int n = g_dxgiLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 32)
        LogF("[DISPQ] %s want=%ux%u -> got=%ux%u hr=0x%08X spoofed=%d", api,
            want ? want->Width : 0u, want ? want->Height : 0u,
            (SUCCEEDED(hr) && got) ? got->Width : 0u, (SUCCEEDED(hr) && got) ? got->Height : 0u,
            static_cast<unsigned>(hr), rewrote ? 1 : 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE FindClosestMatchingModeHook(IDXGIOutput* self, const DXGI_MODE_DESC* want,
    DXGI_MODE_DESC* got, IUnknown* device) noexcept
{
    return ClosestTargetMode("IDXGIOutput::FindClosestMatchingMode", g_origFindClosestMatchingMode,
        self, want, got, device);
}

HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1Hook(IDXGIOutput1* self,
    const DXGI_MODE_DESC1* want, DXGI_MODE_DESC1* got, IUnknown* device) noexcept
{
    return ClosestTargetMode("IDXGIOutput1::FindClosestMatchingMode1",
        g_origFindClosestMatchingMode1, self, want, got, device);
}

bool PatchSlot(void** table, std::size_t index, void* replacement, void** original)
{
    if (!table || !replacement || !original)
        return false;
    void** slot = table + index;
    DWORD oldProtect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    if (*slot != replacement)
    {
        if (!*original)
            *original = *slot;
        InterlockedExchangePointer(slot, replacement);
    }
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}
} // namespace

namespace DisplaySpoof
{
void InstallEarly(HMODULE self)
{
    ReadConfig(self);
    // ffxv_s.exe's manifest declares Per Monitor DPI awareness, so every
    // query below is in physical pixels; say so in the log in case a
    // launcher or manifest change ever flips it (logical sizes would then
    // disagree with EnumDisplaySettings and the spoof would not match).
    {
        const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
        const char* name = "unaware";
        if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
            AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
            name = "per-monitor";
        else if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
            name = "system";
        LogF("[DISPQ] process DPI awareness: %s", name);
    }
    SnapshotDisplays();
    LogF("[DISPQ] primary=%ux%u source=%ux%u target=%ux%u", g_primaryW, g_primaryH, g_sourceW,
        g_sourceH, g_targetW, g_targetH);
    if (!g_targetW || !g_targetH)
    {
        LogLine("[DISPQ] no [Display] BackbufferWH in ffxv-vr.ini; display spoof off");
        return;
    }
    if (!g_sourceW || !g_sourceH)
    {
        LogLine("[DISPQ] no source display found; display spoof off");
        return;
    }
    if (g_targetW * static_cast<unsigned long long>(g_targetH) <=
        g_sourceW * static_cast<unsigned long long>(g_sourceH))
    {
        LogLine("[DISPQ] target is not larger than the source display; display spoof off");
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogF("[DISPQ] MH_Initialize failed: %s; display spoof off", MH_StatusToString(init));
        return;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!user32)
    {
        LogLine("[DISPQ] user32.dll not loaded at attach; display spoof off");
        return;
    }
    // Hooks go live before g_active so a half-installed set never lies.
    bool ok = true;
    ok &= InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW")),
        reinterpret_cast<void*>(&EnumDisplaySettingsWHook),
        reinterpret_cast<void**>(&g_origEnumDisplaySettingsW), "EnumDisplaySettingsW");
    ok &= InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA")),
        reinterpret_cast<void*>(&EnumDisplaySettingsAHook),
        reinterpret_cast<void**>(&g_origEnumDisplaySettingsA), "EnumDisplaySettingsA");
    ok &= InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics")),
        reinterpret_cast<void*>(&GetSystemMetricsHook),
        reinterpret_cast<void**>(&g_origGetSystemMetrics), "GetSystemMetrics");
    ok &= InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoW")),
        reinterpret_cast<void*>(&GetMonitorInfoWHook),
        reinterpret_cast<void**>(&g_origGetMonitorInfoW), "GetMonitorInfoW");
    ok &= InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA")),
        reinterpret_cast<void*>(&GetMonitorInfoAHook),
        reinterpret_cast<void**>(&g_origGetMonitorInfoA), "GetMonitorInfoA");
    InstallHook(reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowPos")),
        reinterpret_cast<void*>(&SetWindowPosHook),
        reinterpret_cast<void**>(&g_origSetWindowPos), "SetWindowPos");
    if (gdi32)
        InstallHook(reinterpret_cast<void*>(GetProcAddress(gdi32, "GetDeviceCaps")),
            reinterpret_cast<void*>(&GetDeviceCapsHook),
            reinterpret_cast<void**>(&g_origGetDeviceCaps), "GetDeviceCaps");
    if (!ok)
    {
        LogLine("[DISPQ] a required display hook failed; display spoof off");
        return;
    }
    g_active.store(true, std::memory_order_release);
    LogF("[DISPQ] display spoof ON: %ux%u reported as %ux%u (native stereo eye = %ux%u before "
        "letterbox)", g_sourceW, g_sourceH, g_targetW, g_targetH, g_targetW / 2, g_targetH);
}

void HookDxgiOutputs(IUnknown* factoryUnknown)
{
    if (!factoryUnknown || !g_active.load(std::memory_order_acquire))
        return;
    if (g_outputsHooked.exchange(true, std::memory_order_acq_rel))
        return;
    IDXGIFactory* factory{};
    if (FAILED(factoryUnknown->QueryInterface(__uuidof(IDXGIFactory),
            reinterpret_cast<void**>(&factory))) || !factory)
    {
        LogLine("[DISPQ] factory has no IDXGIFactory; output hooks skipped");
        return;
    }
    IDXGIOutput* output{};
    for (UINT adapterIndex = 0; adapterIndex < 8 && !output; ++adapterIndex)
    {
        IDXGIAdapter* adapter{};
        if (FAILED(factory->EnumAdapters(adapterIndex, &adapter)) || !adapter)
            break;
        for (UINT outputIndex = 0; outputIndex < 8; ++outputIndex)
        {
            if (FAILED(adapter->EnumOutputs(outputIndex, &output)) || !output)
            {
                output = nullptr;
                break;
            }
            break;
        }
        adapter->Release();
    }
    factory->Release();
    if (!output)
    {
        LogLine("[DISPQ] no IDXGIOutput to hook; DXGI mode lists stay real");
        return;
    }
    // One vtable serves every output object of this dxgi.dll.
    void** table = *reinterpret_cast<void***>(output);
    PatchSlot(table, 7, reinterpret_cast<void*>(&OutputGetDescHook),
        reinterpret_cast<void**>(&g_origOutputGetDesc));
    PatchSlot(table, 8, reinterpret_cast<void*>(&GetDisplayModeListHook),
        reinterpret_cast<void**>(&g_origGetDisplayModeList));
    PatchSlot(table, 9, reinterpret_cast<void*>(&FindClosestMatchingModeHook),
        reinterpret_cast<void**>(&g_origFindClosestMatchingMode));
    IDXGIOutput1* output1{};
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1))) &&
        output1)
    {
        void** table1 = *reinterpret_cast<void***>(output1);
        PatchSlot(table1, 19, reinterpret_cast<void*>(&GetDisplayModeList1Hook),
            reinterpret_cast<void**>(&g_origGetDisplayModeList1));
        PatchSlot(table1, 20, reinterpret_cast<void*>(&FindClosestMatchingMode1Hook),
            reinterpret_cast<void**>(&g_origFindClosestMatchingMode1));
        output1->Release();
    }
    IDXGIOutput6* output6{};
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
        output6)
    {
        void** table6 = *reinterpret_cast<void***>(output6);
        PatchSlot(table6, 27, reinterpret_cast<void*>(&OutputGetDesc1Hook),
            reinterpret_cast<void**>(&g_origOutputGetDesc1));
        output6->Release();
    }
    output->Release();
    LogF("[DISPQ] IDXGIOutput hooks installed (GetDesc%s, mode lists, closest mode)",
        g_origOutputGetDesc1 ? "/GetDesc1" : "");
}

void FlushEarlyLog()
{
    AcquireSRWLockExclusive(&g_earlyLock);
    g_logOpen = true;
    const int count = g_earlyCount;
    g_earlyCount = 0;
    ReleaseSRWLockExclusive(&g_earlyLock);
    for (int index = 0; index < count; ++index)
        RetailLogLine(g_early[index]);
}

bool Active()
{
    return g_active.load(std::memory_order_acquire);
}

void TargetSize(unsigned* width, unsigned* height)
{
    if (width)
        *width = g_targetW;
    if (height)
        *height = g_targetH;
}

void NoteSwapchain(const char* api, unsigned width, unsigned height, unsigned format, int windowed,
    HWND window)
{
    RECT client{};
    if (window)
        GetClientRect(window, &client);
    LogF("[DISPQ] %s desc=%ux%u fmt=%u windowed=%d hwnd=%p client=%ldx%ld spoof=%s target=%ux%u",
        api, width, height, format, windowed, static_cast<void*>(window),
        client.right - client.left, client.bottom - client.top,
        g_active.load(std::memory_order_relaxed) ? "on" : "off", g_targetW, g_targetH);
}
} // namespace DisplaySpoof
