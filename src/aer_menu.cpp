#include "research_diagnostics.h"
#include "gpu_pose_trace.h"
#include "openxr_presenter.h"
#include "aer_menu.h"
#include "ui_hook.h"
#include "dlss_upscaler.h"
#include "mv_fix.h"

#include "aer_control.h"
#include "runtime_log.h"

#include <windows.h>
#include <xinput.h>

#include <cstdlib>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <MinHook.h>

#include <algorithm>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
std::atomic_bool g_visible{false};
bool g_initialized{};
bool g_initializationFailed{};
bool g_insertWasDown{};
bool g_recenterWasDown{};
bool g_imageHoldWasDown{};
bool g_markWasDown{};
bool g_customCameraWasDown{};
// [KEYBIND] Shared rebind capture. While g_captureTarget points at
// a key, the next WM_KEYDOWN this window sees (MenuWndProc, below) is stolen
// before ImGui or the game gets it and written into *g_captureTarget instead.
// [MENUINPUT] the row being rebound, by its id string (a literal, so the
// address is stable).  It used to point at a per-frame local int, which only
// matched again next frame if the stack happened to line up.
std::atomic<const void*> g_captureTarget{nullptr};
std::atomic<int> g_capturedVk{0};
int g_recenterKey = 'R';
std::atomic_int g_menuKey{VK_INSERT};   // [MENUKEY] opens / closes this menu
int g_firstPersonKey = 'K';
// [VRHINT] load-time notification: on by default (a fresh install always
// shows it once), one Insert-menu checkbox to turn it off for good.
bool g_showVrHint = true;
// [DRAGFIX] the menu polls its own mouse instead of trusting the real OS
// cursor. The game clips/re-centres the real cursor for its own camera look,
// which starves ImGui of real WM_MOUSEMOVE messages until something (e.g. the
// Windows key) forces Windows to release that capture. Pinning the real
// cursor to one spot every frame and feeding ImGui only the delta since last
// frame is immune to whatever the game is doing with the cursor.
// ---- [MENUINPUT] Menu input model ------------
// While the menu is open and the game window is in front, low-level mouse and
// keyboard hooks (own thread + message pump) read the mouse and swallow it and
// every key except the menu key, so neither the game nor Windows sees them.
// The game cannot re-pin or clip the cursor meanwhile (SetCursorPos /
// ClipCursor hooked).  The menu's cursor moves by the raw movement and is kept
// inside the menu, so it never hangs at a screen edge, never needs the
// Windows key, and never disappears off the panel.
std::atomic_bool g_inputActive{false};
volatile LONG g_llDX = 0, g_llDY = 0, g_llLBtn = 0, g_llRBtn = 0, g_llWheel = 0;
HHOOK g_mouseHook{}, g_keyHook{};
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
SetCursorPosFn g_realSetCursorPos{};
ClipCursorFn g_realClipCursor{};
float g_cursorX = 0.0f, g_cursorY = 0.0f;
bool g_cursorPlaced = false;
bool g_dragActive = false;   // the drag bar is being held
POINT g_fallbackLast{};
HWND g_window{};
WNDPROC g_originalWndProc{};
ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
ID3D11Texture2D* g_backBuffer{};
ID3D11RenderTargetView* g_renderTarget{};
wchar_t g_iniPath[MAX_PATH]{};
// [MENUQUAD] offscreen menu texture (same size as the backbuffer, FP16 like
// the interface layer) and the menu window rectangle of the last frame.
ID3D11Texture2D* g_menuTexture{};
ID3D11RenderTargetView* g_menuTarget{};
UINT g_menuW{}, g_menuH{};
int g_menuRect[4]{};
bool g_advanced{};   // [MENU2] ini [AER] Advanced: experiments shown in the menu

void ReleaseRenderTarget()
{
    if (g_renderTarget)
    {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
    if (g_backBuffer)
    {
        g_backBuffer->Release();
        g_backBuffer = nullptr;
    }
}

bool IsKeyboardMessage(UINT message)
{
    return message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_CHAR;
}

bool IsMouseMessage(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
}

LRESULT CALLBACK MenuWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // [MENUINPUT] while the menu is open the game gets no mouse or keyboard
    // messages; the menu reads its own input (hooks below).
    if (g_initialized && g_visible.load(std::memory_order_acquire))
    {
        switch (message)
        {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            // Backup rebind capture, in case the low-level hook is missing.
            if (g_captureTarget.load(std::memory_order_acquire) != nullptr)
            {
                const int vk = static_cast<int>(wParam);
                if (vk == VK_ESCAPE)
                    g_captureTarget.store(nullptr, std::memory_order_release);
                else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU)
                    g_capturedVk.store(vk, std::memory_order_release);
            }
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            return 0;
        case WM_INPUT:
            return DefWindowProcW(window, message, wParam, lParam);   // cleanup only
        default:
            break;
        }
    }
    return g_originalWndProc
        ? CallWindowProcW(g_originalWndProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

// Checked on every event, never cached: the hooks let everything through the
// moment the menu closes or the game window is not in front.
bool InputActiveNow()
{
    return g_inputActive.load(std::memory_order_acquire) &&
        g_visible.load(std::memory_order_acquire) &&
        g_window && GetForegroundWindow() == g_window;
}

LRESULT CALLBACK MenuMouseLL(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && InputActiveNow())
    {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        switch (wParam)
        {
        case WM_MOUSEMOVE:
        {
            POINT now{};
            if (m && GetCursorPos(&now))
            {
                InterlockedExchangeAdd(&g_llDX, m->pt.x - now.x);
                InterlockedExchangeAdd(&g_llDY, m->pt.y - now.y);
            }
            return 1;
        }
        case WM_LBUTTONDOWN: InterlockedExchange(&g_llLBtn, 1); return 1;
        case WM_LBUTTONUP:   InterlockedExchange(&g_llLBtn, 0); return 1;
        case WM_RBUTTONDOWN: InterlockedExchange(&g_llRBtn, 1); return 1;
        case WM_RBUTTONUP:   InterlockedExchange(&g_llRBtn, 0); return 1;
        case WM_MOUSEWHEEL:
            if (m) InterlockedExchangeAdd(&g_llWheel, static_cast<SHORT>(HIWORD(m->mouseData)));
            return 1;
        default:
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK MenuKeyLL(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && InputActiveNow())
    {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const int vk = k ? static_cast<int>(k->vkCode) : 0;
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        // Never block the system: Windows and Alt (Alt+Tab).  Shift and Ctrl
        // are game keys (sprint, etc.) and are blocked like any other.
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
            return CallNextHookEx(nullptr, code, wParam, lParam);
        // [MENUBLOCK] Key releases always reach the game.  A key held when the
        // menu opened would otherwise stay "down" in the game (Noctis keeps
        // running); a release of a key it never saw pressed is harmless.
        if (!down && g_captureTarget.load(std::memory_order_acquire) == nullptr)
            return CallNextHookEx(nullptr, code, wParam, lParam);
        // Rebind capture happens here, before the key is swallowed.
        if (g_captureTarget.load(std::memory_order_acquire) != nullptr)
        {
            if (down && vk)
            {
                if (vk == VK_ESCAPE)
                    g_captureTarget.store(nullptr, std::memory_order_release);
                else
                    g_capturedVk.store(vk, std::memory_order_release);
            }
            return 1;
        }
        if (vk == g_menuKey.load(std::memory_order_relaxed))
            return CallNextHookEx(nullptr, code, wParam, lParam);   // must still close the menu
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI MenuInputThread(LPVOID)
{
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&MenuMouseLL), &self);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &MenuMouseLL, self, 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &MenuKeyLL, self, 0);
    char line[128];
    std::snprintf(line, sizeof(line), "[MENUINPUT] low-level hooks mouse=%s keyboard=%s",
        g_mouseHook ? "ready" : "FAILED", g_keyHook ? "ready" : "FAILED");
    RetailLogLine(line);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

BOOL WINAPI HookedSetCursorPos(int x, int y)
{
    if (InputActiveNow())
        return TRUE;
    return g_realSetCursorPos ? g_realSetCursorPos(x, y) : FALSE;
}

BOOL WINAPI HookedClipCursor(const RECT* rect)
{
    if (InputActiveNow())
        return TRUE;
    return g_realClipCursor ? g_realClipCursor(rect) : FALSE;
}

// [MENUBLOCK] The game reads the Xbox pad through XInputGetState
// (xinput9_1_0.dll).  While the menu is open it gets a connected pad with
// nothing pressed and sticks centred, so Noctis stands still.  The packet
// number keeps counting, so the game never sees the pad as frozen.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_realXInputGetState[3]{};
std::atomic_uint64_t g_padBlocked{};
template<int Slot>
DWORD WINAPI HookedXInputGetState(DWORD user, XINPUT_STATE* state)
{
    const DWORD result = g_realXInputGetState[Slot] ? g_realXInputGetState[Slot](user, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (result == ERROR_SUCCESS && state && g_visible.load(std::memory_order_acquire))
    {
        state->Gamepad = XINPUT_GAMEPAD{};
        if (g_padBlocked.fetch_add(1, std::memory_order_relaxed) == 0)
            RetailLogLine("[MENUBLOCK] gamepad input held from the game while the menu is open");
    }
    return result;
}

void InstallPadBlock()
{
    const wchar_t* names[3] = {L"xinput9_1_0.dll", L"xinput1_4.dll", L"xinput1_3.dll"};
    void* hooks[3] = {reinterpret_cast<void*>(&HookedXInputGetState<0>),
        reinterpret_cast<void*>(&HookedXInputGetState<1>), reinterpret_cast<void*>(&HookedXInputGetState<2>)};
    char line[160];
    for (int i = 0; i < 3; ++i)
    {
        // Only DLLs the game has loaded (or loads for its own imports).
        HMODULE module = GetModuleHandleW(names[i]);
        if (!module && i == 0)
            module = LoadLibraryW(names[i]);
        if (!module)
            continue;
        void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
        MH_STATUS status = MH_ERROR_FUNCTION_NOT_FOUND;
        if (target)
        {
            status = MH_CreateHook(target, hooks[i], reinterpret_cast<void**>(&g_realXInputGetState[i]));
            if (status == MH_OK)
                status = MH_EnableHook(target);
        }
        std::snprintf(line, sizeof(line), "[MENUBLOCK] %ls XInputGetState %s", names[i],
            status == MH_OK ? "hooked" : MH_StatusToString(status));
        RetailLogLine(line);
    }
}

void InstallMenuInput()
{
    if (HANDLE thread = CreateThread(nullptr, 0, &MenuInputThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* setCursor = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")) : nullptr;
    void* clipCursor = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")) : nullptr;
    bool ok = false;
    if (setCursor && MH_CreateHook(setCursor, reinterpret_cast<void*>(&HookedSetCursorPos),
            reinterpret_cast<void**>(&g_realSetCursorPos)) == MH_OK && MH_EnableHook(setCursor) == MH_OK)
        ok = true;
    if (clipCursor && MH_CreateHook(clipCursor, reinterpret_cast<void*>(&HookedClipCursor),
            reinterpret_cast<void**>(&g_realClipCursor)) == MH_OK && MH_EnableHook(clipCursor) == MH_OK)
        ok = ok && true;
    else
        ok = false;
    RetailLogLine(ok ? "[MENUINPUT] game cursor pin/clip blocked while the menu is open"
                     : "[MENUINPUT] cursor hooks NOT installed");
    InstallPadBlock();   // [MENUBLOCK]
}

bool RefreshRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer{};
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
        return false;

    if (backBuffer == g_backBuffer && g_renderTarget)
    {
        backBuffer->Release();
        return true;
    }

    ReleaseRenderTarget();
    g_backBuffer = backBuffer;
    if (FAILED(g_device->CreateRenderTargetView(g_backBuffer, nullptr, &g_renderTarget)))
    {
        ReleaseRenderTarget();
        return false;
    }
    return true;
}

void BuildIniPath()
{
    HMODULE module{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&BuildIniPath), &module))
        return;
    const DWORD length = GetModuleFileNameW(module, g_iniPath, MAX_PATH);
    if (!length || length >= MAX_PATH)
    {
        g_iniPath[0] = L'\0';
        return;
    }
    wchar_t* slash = std::wcsrchr(g_iniPath, L'\\');
    if (!slash)
    {
        g_iniPath[0] = L'\0';
        return;
    }
    std::wcsncpy(slash + 1, L"ffxv-vr.ini", MAX_PATH - (slash + 1 - g_iniPath));
    g_iniPath[MAX_PATH - 1] = L'\0';
}

float ReadFloat(const wchar_t* key, float fallback)
{
    if (!g_iniPath[0])
        return fallback;
    wchar_t fallbackText[32]{};
    wchar_t text[32]{};
    swprintf_s(fallbackText, L"%.6f", fallback);
    GetPrivateProfileStringW(L"AER", key, fallbackText, text,
        static_cast<DWORD>(std::size(text)), g_iniPath);
    wchar_t* end{};
    const float value = std::wcstof(text, &end);
    return end != text && std::isfinite(value) ? value : fallback;
}

bool ReadBool(const wchar_t* key, bool fallback)
{
    return g_iniPath[0]
        ? GetPrivateProfileIntW(L"AER", key, fallback ? 1 : 0, g_iniPath) != 0
        : fallback;
}

void WriteFloat(const wchar_t* key, float value)
{
    if (!g_iniPath[0])
        return;
    wchar_t text[32]{};
    swprintf_s(text, L"%.6f", value);
    WritePrivateProfileStringW(L"AER", key, text, g_iniPath);
}

void WriteBool(const wchar_t* key, bool value)
{
    if (g_iniPath[0])
        WritePrivateProfileStringW(L"AER", key, value ? L"1" : L"0", g_iniPath);
}

// A UTF-8 byte-order mark at the head of the ini makes GetPrivateProfileStringW
// fail to find the first section, and every single setting then silently reads
// as its default: the settings file can look perfectly
// correct in an editor while nothing is applied.  An editor or
// script that saves "UTF-8" rather than "UTF-8 without BOM" is enough to do it,
// so heal the file instead of trusting it.
void StripIniBom()
{
    if (!g_iniPath[0])
        return;
    HANDLE file = CreateFileW(g_iniPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    unsigned char head[3]{};
    DWORD read = 0;
    if (ReadFile(file, head, sizeof(head), &read, nullptr) && read == 3 &&
        head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
    {
        const DWORD size = GetFileSize(file, nullptr);
        if (size != INVALID_FILE_SIZE && size > 3 && size < (1u << 20))
        {
            const DWORD rest = size - 3;
            auto* body = static_cast<char*>(std::malloc(rest));
            if (body)
            {
                DWORD got = 0;
                SetFilePointer(file, 3, nullptr, FILE_BEGIN);
                if (ReadFile(file, body, rest, &got, nullptr) && got == rest)
                {
                    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
                    DWORD written = 0;
                    WriteFile(file, body, rest, &written, nullptr);
                    SetEndOfFile(file);
                    RetailLogLine("[AERUI] stripped a UTF-8 BOM from the settings file; "
                        "it was hiding every setting");
                }
                std::free(body);
            }
        }
    }
    CloseHandle(file);
}

float ReadDisplayFloat(const wchar_t* key, float fallback)
{
    if (!g_iniPath[0]) return fallback;
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"Display", key, L"", text, 32, g_iniPath);
    if (!text[0]) return fallback;
    return static_cast<float>(_wtof(text));
}
void WriteDisplayFloat(const wchar_t* key, float value)
{
    if (!g_iniPath[0]) return;
    wchar_t text[32]{};
    swprintf_s(text, L"%.2f", value);
    WritePrivateProfileStringW(L"Display", key, text, g_iniPath);
}
int ReadDisplayInt(const wchar_t* key, int fallback)
{
    if (!g_iniPath[0]) return fallback;
    return GetPrivateProfileIntW(L"Display", key, fallback, g_iniPath);
}
void WriteDisplayInt(const wchar_t* key, int value)
{
    if (!g_iniPath[0]) return;
    wchar_t text[16]{};
    swprintf_s(text, L"%d", value);
    WritePrivateProfileStringW(L"Display", key, text, g_iniPath);
}
// [KEYBIND] "F9", "R", "A", ... for the rebind rows.
const char* VkName(int vk)
{
    static char buf[32];
    switch (vk)
    {
    case VK_INSERT: return "Insert";
    case VK_SPACE:  return "Space";
    case VK_TAB:    return "Tab";
    case VK_RETURN: return "Enter";
    case VK_PAUSE:  return "Pause";
    default: break;
    }
    if (vk >= '0' && vk <= '9') { buf[0] = static_cast<char>(vk); buf[1] = 0; return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = static_cast<char>(vk); buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) { std::snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { std::snprintf(buf, sizeof(buf), "Numpad %d", vk - VK_NUMPAD0); return buf; }
    // [KEYNAME] anything else: ask Windows for the key's own name.
    UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    LONG lparam = static_cast<LONG>((scan & 0xFF) << 16);
    if ((scan & 0xFF00) == 0xE000 || (scan & 0xFF00) == 0xE100)
        lparam |= 1 << 24;
    if (scan && GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0)
        return buf;
    std::snprintf(buf, sizeof(buf), "key 0x%02X", vk);
    return buf;
}
// "Key: X [Rebind] [Reset]" row.  Returns true the frame a rebind or reset
// commits, so the caller can persist it the same way every other control
// in this file does.
// [KEYBIND2] Capture no
// longer depends on WM_KEYDOWN reaching the mod's window procedure (the game may
// take raw keyboard input).  While armed, a newly pressed key found by polling
// counts too.  Mouse buttons and Insert (the menu key) are never captured.
bool g_capturePrevDown[256]{};
bool g_capturePollPrimed = false;
int PollCapturedKey()
{
    int found = 0;
    for (int vk = 0x08; vk < 0xFF; ++vk)
    {
        if (vk == g_menuKey.load() || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_capturePrevDown[vk] && g_capturePollPrimed && !found)
            found = vk;
        g_capturePrevDown[vk] = down;
    }
    g_capturePollPrimed = true;
    return found;
}

bool KeyRebindRow(const char* label, int* key, int def, const char* id)
{
    bool changed = false;
    if (g_captureTarget.load(std::memory_order_acquire) == id)
    {
        if (!g_capturedVk.load(std::memory_order_acquire))
        {
            const int polled = PollCapturedKey();
            if (polled == VK_ESCAPE)
                g_captureTarget.store(nullptr, std::memory_order_release);
            else if (polled)
                g_capturedVk.store(polled, std::memory_order_release);
        }
        const int vk = g_capturedVk.exchange(0, std::memory_order_acq_rel);
        if (vk != 0)
        {
            *key = vk;
            g_captureTarget.store(nullptr, std::memory_order_release);
            changed = true;
        }
    }
    ImGui::Text("%s: %s", label, VkName(*key));
    ImGui::SameLine();
    ImGui::PushID(id);
    if (g_captureTarget.load(std::memory_order_acquire) == id)
        ImGui::TextDisabled("press a key (Esc cancels)...");
    else
    {
        if (ImGui::Button("Rebind"))
        {
            g_capturedVk.store(0, std::memory_order_release);
            g_capturePollPrimed = false;   // keys already held do not count
            g_captureTarget.store(id, std::memory_order_release);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset") && *key != def)
        {
            *key = def;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

void LoadSettings()
{
    BuildIniPath();
    StripIniBom();
    RetailXr::SetSharpen(ReadDisplayFloat(L"Sharpen", 0.25f));   // [DEFAULTS]
    UiHook::SetTextureBias(ReadDisplayFloat(L"TextureBias", -0.50f));
    MvFix::g_dlssFix.store(ReadDisplayFloat(L"DlssMotionFix", 1.0f) > 0.5f);
    RetailXr::SetMenuWidth(ReadDisplayFloat(L"MenuWidth", 0.75f));
    AerControl::SetHalfEyeMeters(ReadFloat(L"HalfEyeMeters", AerControl::GetHalfEyeMeters()));
    AerControl::SetConvergence(ReadFloat(L"Convergence", AerControl::GetConvergence()));
    AerControl::SetFillFactor(ReadFloat(L"FillFactor", AerControl::GetFillFactor()));
    AerControl::SetLiveFovDegrees(ReadFloat(L"LiveFovDegrees", 87.0f));
    AerControl::SetFovParamIndex(
        static_cast<int>(ReadFloat(L"FovParamIndex", -1.0f)));
    AerControl::SetFovAllModes(ReadBool(L"FovAllModes", false));
    AerControl::SetEyeHoldEnabled(ReadBool(L"EyeHold", true));
    AerControl::SetAerSimSync(ReadBool(L"SimSync", true));
    AerControl::SetAerFreshHeadPose(ReadBool(L"AerFreshHeadPose", false));
    AerControl::SetDecoupledPitch(ReadBool(L"DecoupledPitch", false));
    RetailXr::SetDockFlatScreens(ReadBool(L"DockFlatScreens", false));
    AerControl::SetLensFeedEnabled(ReadBool(L"LensFeed", true));
    AerControl::SetFovSink(
        static_cast<int>(ReadFloat(L"FovSinkOffset", -1.0f)),
        static_cast<int>(ReadFloat(L"FovSinkKind", 0.0f)));
    AerControl::SetSwapEyes(ReadBool(L"SwapEyes", AerControl::GetSwapEyes()));
    AerControl::SetAfwEnabled(ReadBool(L"Afw", false));
    AerControl::SetAfwFlip(ReadBool(L"AfwFlip", false));
    AerControl::SetNativeHeadTiming(ReadFloat(L"NativeHeadTiming", 0.0f));
    // [LOOKAHEAD] default 0 (1-2 frames of lookahead adds jitter).
    // -1 = auto (measured depth) stays available.
    AerControl::SetHeadLookaheadFrames(ReadFloat(L"HeadLookahead", 0.0f));
    // [TAGMODE] default current: the exact FIFO tag adds jitter.
    AerControl::SetNativeTagCurrent(ReadBool(L"NativeTagCurrent", true));
    GpuPoseTrace::SetEnabled(ResearchDiagnostics::Enabled && ReadBool(L"HeadFrameTrace", false));
    AerControl::SetSixDofEnabled(ReadBool(L"SixDofEnabled", true));
    AerControl::SetSixDofPositionScale(ReadFloat(L"SixDofPositionScale", 1.0f));
    AerControl::SetFlatSceneMonoEnabled(ReadBool(L"FlatSceneMono", true));
    // Default ON.
    AerControl::SetUiFitEnabled(ReadBool(L"UiFit", true));
    AerControl::SetUiFit(
        ReadFloat(L"UiFitScaleX", 0.85f), ReadFloat(L"UiFitScaleY", 0.85f),
        ReadFloat(L"UiFitOffsetX", 0.0f), ReadFloat(L"UiFitOffsetY", 0.0f));
    AerControl::SetNativeBaselineCm(ReadFloat(L"NativeBaselineCm", 0.0f));
    // Native rig keys.  The older NativeSwapEyes / NativeConvergence /
    // NativeSymmetric keys belong to a previous rig layout (48-degree lens
    // shown at 117 degrees, engine skew on); they are left untouched in the
    // file and not read.  NativeSeatSwapEyes: the right-eye seat moves along
    // the true camera right.
    AerControl::SetNativeSwapEyes(ReadBool(L"NativeSeatSwapEyes", true));
    AerControl::SetNativeConvergence(ReadFloat(L"NativeRigConvergence", -0.0081f));
    AerControl::SetNativeFill(ReadFloat(L"NativeFillFactor", 1.0f));
    AerControl::SetNativeSymmetric(ReadBool(L"NativeRigSymmetric", true));   // [MESWAP] symmetric rig: swap has no lateral shift
    g_advanced = ReadBool(L"Advanced", false);
    AerControl::SetConversationMode(static_cast<int>(ReadFloat(L"ConversationMode", 0.0f)));
    AerControl::SetHudLayerEnabled(ReadBool(L"HudLayer", true));
    AerControl::SetHudLensExact(ReadBool(L"HudLensExact", true));
    AerControl::SetHudWorldDistance(ReadFloat(L"HudWorldDistance", 20.0f));
    AerControl::SetHudWorldBaseCamera(ReadBool(L"HudWorldBaseCamera", false));
    AerControl::SetHudLayerParams(ReadFloat(L"HudLayerDistance", 1.5f),
        ReadFloat(L"HudLayerWidth", 1.6f), ReadFloat(L"HudLayerHeight", 0.0f));
    AerControl::SetCustomFirstPersonOffsets(
        ReadFloat(L"FirstPersonRight", 0.0f),
        ReadFloat(L"FirstPersonUp", 0.0f),
        ReadFloat(L"FirstPersonForward", 0.0f));
    // Manual per-session modes: never move or globally re-project a camera on startup.
    AerControl::SetCustomFirstPersonEnabled(false);
    g_showVrHint = ReadBool(L"ShowVrHint", true);
    RetailXr::SetMenuOffset(ReadDisplayFloat(L"MenuOffsetX", 0.0f), ReadDisplayFloat(L"MenuOffsetY", 0.0f));
    g_recenterKey = ReadDisplayInt(L"RecenterKey", 'R');
    g_firstPersonKey = ReadDisplayInt(L"FirstPersonKey", 'K');
    AerControl::SetVrEnableKey(ReadDisplayInt(L"VrEnableKey", VK_F9));
    AerControl::SetMonoKey(ReadDisplayInt(L"MonoKey", VK_END));
    g_menuKey = ReadDisplayInt(L"MenuKey", VK_INSERT);
    AerControl::SetPreferredVrMode(ReadDisplayInt(L"VrMode", 1));
    AerControl::SetLiveFovOverrideEnabled(false);
}

// ---- [SNAP] Numpad 9 saves the next 12 consecutive frames of the
// game's own side-by-side image (both eyes, after DLSS and post) as BMPs at
// half size, so a blink that lasts a frame or two can be seen, not described.
// Files: <game>\vrsnap\mark<N>_f<00..11>.bmp.  Staging textures are created on
// the first mark and kept.
constexpr int kSnapFrames = 12;
ID3D11Texture2D* g_snapStaging[kSnapFrames]{};
UINT g_snapW{}, g_snapH{};
DXGI_FORMAT g_snapFormat{};
int g_snapCaptured = -1;   // -1 idle, 0..kSnapFrames capturing
int g_snapWait = 0;
unsigned g_snapMark = 0;

float SnapHalf(std::uint16_t h)
{
    const std::uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = 65504.0f;
    else v = std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}
void SnapPixel(const std::uint8_t* px, DXGI_FORMAT f, float rgb[3])
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2]; return;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0]; return;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        std::uint32_t v; std::memcpy(&v, px, 4);
        rgb[0] = static_cast<float>(v & 1023) / 4.0f;
        rgb[1] = static_cast<float>((v >> 10) & 1023) / 4.0f;
        rgb[2] = static_cast<float>((v >> 20) & 1023) / 4.0f;
        return;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    {
        std::uint16_t h[3]; std::memcpy(h, px, 6);
        for (int i = 0; i < 3; ++i)
        {
            const float lin = SnapHalf(h[i]);
            rgb[i] = 255.0f * std::pow(std::clamp(lin, 0.0f, 1.0f), 1.0f / 2.2f);
        }
        return;
    }
    default:
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
    }
}
UINT SnapBytesPerPixel(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}
bool SnapWrite(int frame, ID3D11Texture2D* staging)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        return false;
    const UINT ow = g_snapW / 2, oh = g_snapH / 2;
    const UINT rowBytes = (ow * 3 + 3) & ~3u;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(rowBytes) * oh);
    const UINT bpp = SnapBytesPerPixel(g_snapFormat);
    for (UINT y = 0; y < oh; ++y)
    {
        const auto* r0 = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(2 * y) * m.RowPitch;
        const auto* r1 = r0 + m.RowPitch;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(oh - 1 - y) * rowBytes;   // bottom-up
        for (UINT x = 0; x < ow; ++x)
        {
            float a[3], b[3], c[3], d[3];
            SnapPixel(r0 + (2 * x) * bpp, g_snapFormat, a);
            SnapPixel(r0 + (2 * x + 1) * bpp, g_snapFormat, b);
            SnapPixel(r1 + (2 * x) * bpp, g_snapFormat, c);
            SnapPixel(r1 + (2 * x + 1) * bpp, g_snapFormat, d);
            for (int k = 0; k < 3; ++k)
            {
                const float v = (a[k] + b[k] + c[k] + d[k]) * 0.25f;
                dst[x * 3 + (2 - k)] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));   // BGR
            }
        }
    }
    g_context->Unmap(staging, 0);

    wchar_t dir[MAX_PATH]{};
    std::wcsncpy(dir, g_iniPath, MAX_PATH - 1);
    if (wchar_t* slash = std::wcsrchr(dir, L'\\')) *(slash + 1) = L'\0';
    std::wcsncat(dir, L"vrsnap", MAX_PATH - 1 - std::wcslen(dir));
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%s\\mark%u_f%02d.bmp", dir, g_snapMark, frame);
    FILE* file{};
    if (_wfopen_s(&file, path, L"wb") != 0 || !file)
        return false;
    const std::uint32_t imageSize = static_cast<std::uint32_t>(out.size());
    std::uint8_t header[54]{};
    header[0] = 'B'; header[1] = 'M';
    const std::uint32_t fileSize = 54 + imageSize, offset = 54, infoSize = 40;
    const std::int32_t w = static_cast<std::int32_t>(ow), h = static_cast<std::int32_t>(oh);
    const std::uint16_t planes = 1, bits = 24;
    std::memcpy(header + 2, &fileSize, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &infoSize, 4);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &imageSize, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    return true;
}
void SnapBegin()
{
    if (g_snapCaptured >= 0)
        return;
    g_snapCaptured = 0;
    ++g_snapMark;
}
void SnapOnPresent(IDXGISwapChain* swapChain)
{
    if (g_snapCaptured < 0 || !g_device || !g_context)
        return;
    if (g_snapCaptured < kSnapFrames)
    {
        ID3D11Texture2D* back{};
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
        {
            g_snapCaptured = -1;
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        if (g_snapCaptured == 0 && (d.Width != g_snapW || d.Height != g_snapH || d.Format != g_snapFormat || !g_snapStaging[0]))
        {
            for (auto*& t : g_snapStaging) if (t) { t->Release(); t = nullptr; }
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            bool ok = true;
            for (auto*& t : g_snapStaging)
                ok = ok && SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &t));
            g_snapW = d.Width; g_snapH = d.Height; g_snapFormat = d.Format;
            if (!ok)
            {
                RetailLogLine("[SNAP] could not create staging textures; no frames saved");
                back->Release();
                g_snapCaptured = -1;
                return;
            }
        }
        g_context->CopyResource(g_snapStaging[g_snapCaptured], back);
        back->Release();
        if (++g_snapCaptured == kSnapFrames)
            g_snapWait = 2;
        return;
    }
    if (--g_snapWait > 0)
        return;
    int saved = 0;
    for (int i = 0; i < kSnapFrames; ++i)
        saved += SnapWrite(i, g_snapStaging[i]) ? 1 : 0;
    char line[160];
    std::snprintf(line, sizeof(line), "[SNAP] mark %u: saved %d of %d consecutive frames (%ux%u fmt %d, half size) to vrsnap\\mark%u_f*.bmp",
        g_snapMark, saved, kSnapFrames, g_snapW, g_snapH, static_cast<int>(g_snapFormat), g_snapMark);
    RetailLogLine(line);
    g_snapCaptured = -1;
}

bool EnsureMenuTexture(IDXGISwapChain* swapChain)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    if (!swapChain || FAILED(swapChain->GetDesc(&sd)) || !g_device)
        return false;
    const UINT w = sd.BufferDesc.Width, h = sd.BufferDesc.Height;
    if (g_menuTexture && g_menuTarget && g_menuW == w && g_menuH == h)
        return true;
    if (g_menuTarget) { g_menuTarget->Release(); g_menuTarget = nullptr; }
    if (g_menuTexture) { g_menuTexture->Release(); g_menuTexture = nullptr; }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_menuTexture)) ||
        FAILED(g_device->CreateRenderTargetView(g_menuTexture, nullptr, &g_menuTarget)))
    {
        if (g_menuTexture) { g_menuTexture->Release(); g_menuTexture = nullptr; }
        RetailLogLine("[MENUQUAD] menu texture creation failed; menu stays on the desktop image");
        return false;
    }
    g_menuW = w; g_menuH = h;
    char line[96];
    std::snprintf(line, sizeof(line), "[MENUQUAD] menu texture %ux%u ready; headset menu on its own panel", w, h);
    RetailLogLine(line);
    return true;
}

bool Initialize(IDXGISwapChain* swapChain)
{
    if (!swapChain || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device)
        return false;
    g_device->GetImmediateContext(&g_context);
    if (!g_context)
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow)
        return false;
    g_window = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // [CLICKFIX] the backend still posts the real (pinned) cursor once per
    // frame before ApplyVirtualMouse; without trickling the LAST position of
    // the frame (the virtual one) is the one a click lands on.
    io.ConfigInputTrickleEventQueue = false;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.6f);
    style.WindowRounding = 7.0f;
    style.FrameRounding = 4.0f;
    io.FontGlobalScale = 1.6f;

    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context) ||
        !RefreshRenderTarget(swapChain))
        return false;

    SetLastError(0);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
    if (!g_originalWndProc && GetLastError() != 0)
        return false;

    LoadSettings();
    InstallMenuInput();   // [MENUINPUT]
    g_initialized = true;
    RetailLogLine("[AERUI] Insert menu initialized (pre-OpenXR headset capture)");
    return true;
}

// ---- [MENU2] Menu layout -----------------------
// One fixed-size panel in the headset (no title bar, no resize), tabs, a
// CollapsingHeader per topic, one control per line with its own Reset, one
// line of help at most.  Only the controls of the ACTIVE stereo mode are
// shown; experiments live behind "Show advanced settings".
constexpr float kControlWidth = 400.0f;

bool ResetButton(const char* id)
{
    ImGui::SameLine();
    ImGui::PushID(id);
    const bool pressed = ImGui::SmallButton("Reset");
    ImGui::PopID();
    return pressed;
}
// Slider + Reset on one line; true when the value changed (slider or reset).
// [MENULOG] every change is logged, so a crash log shows what was touched.
void LogMenuChange(const char* label, float value)
{
    char line[160];
    std::snprintf(line, sizeof(line), "[MENU] %s = %.4f", label, value);
    RetailLogLine(line);
}
bool SliderReset(const char* label, float* value, float lo, float hi, const char* fmt, float def)
{
    ImGui::SetNextItemWidth(kControlWidth);
    bool changed = ImGui::SliderFloat(label, value, lo, hi, fmt);
    if (ResetButton(label))
    {
        *value = def;
        changed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()))
        LogMenuChange(label, *value);   // once, when the drag ends (or on Reset)
    return changed;
}
bool CheckReset(const char* label, bool* value, bool def)
{
    bool changed = ImGui::Checkbox(label, value);
    if (ResetButton(label))
    {
        *value = def;
        changed = true;
    }
    if (changed)
        LogMenuChange(label, *value ? 1.0f : 0.0f);
    return changed;
}
void Help(const char* text)
{
    ImGui::TextDisabled("%s", text);
}
void StatusLine(bool good, const char* text)
{
    ImGui::TextColored(good ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.65f, 0.30f, 1.0f), "%s", text);
}

void DrawStereoNative()
{
    if (!ImGui::CollapsingHeader("Stereo", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    constexpr float kNeutralHalf = 0.032f;   // 64 mm = world scale 1.00
    float mm = AerControl::GetNativeBaselineCm() * 10.0f;
    if (SliderReset("Stereo separation", &mm, 20.0f, 400.0f, "%.0f mm", 60.0f))
    {
        AerControl::SetNativeBaselineCm(mm / 10.0f);
        WriteFloat(L"NativeBaselineCm", AerControl::GetNativeBaselineCm());
    }
    const float half = AerControl::GetNativeBaselineCm() * 0.005f;
    float scale = half > 0.0001f ? kNeutralHalf / half : 1.0f;
    if (SliderReset("World scale", &scale, 0.16f, 3.20f, "%.2fx", kNeutralHalf / 0.03f))
    {
        AerControl::SetNativeBaselineCm(kNeutralHalf / scale * 200.0f);
        WriteFloat(L"NativeBaselineCm", AerControl::GetNativeBaselineCm());
    }
    Help("Linked: more separation makes the world smaller. Stock is 60 mm.");
    float convergence = AerControl::GetNativeConvergence();
    if (SliderReset("Convergence", &convergence, -0.150f, 0.150f, "%+.4f", -0.0081f))
    {
        AerControl::SetNativeConvergence(convergence);
        WriteFloat(L"NativeRigConvergence", AerControl::GetNativeConvergence());
    }
    bool swap = AerControl::GetNativeSwapEyes();
    if (CheckReset("Swap eyes", &swap, true))
    {
        AerControl::SetNativeSwapEyes(swap);
        WriteBool(L"NativeSeatSwapEyes", swap);
    }
    if (g_advanced)
    {
        float fill = AerControl::GetNativeFill();
        if (SliderReset("Headset fill", &fill, 0.40f, 1.80f, "%.2fx", 1.0f))
        {
            AerControl::SetNativeFill(fill);
            WriteFloat(L"NativeFillFactor", AerControl::GetNativeFill());
        }
        Help("1.00 claims exactly the lens the game rendered.");
    }
    ImGui::Spacing();
    int conversationMode = AerControl::GetConversationMode();
    const char* const kConversationModes[] = {
        "Keep stereo (may hang)", "Mono (one output)", "Switch to AER"};
    ImGui::SetNextItemWidth(kControlWidth);
    if (ImGui::Combo("During conversations", &conversationMode, kConversationModes, 3))
    {
        AerControl::SetConversationMode(conversationMode);
        WriteFloat(L"ConversationMode", static_cast<float>(conversationMode));
    }
    if (const int active = AerControl::GetConversationActive())
        StatusLine(true, active == 1 ? "Conversation running: mono right now" : "Conversation running: AER right now");
    int monoKey = AerControl::GetMonoKey();
    if (KeyRebindRow("Switch to Mono key", &monoKey, VK_END, "monoKeyRow"))
    {
        AerControl::SetMonoKey(monoKey);
        WriteDisplayInt(L"MonoKey", monoKey);
    }
    {
        char hint[96];
        std::snprintf(hint, sizeof(hint), "If the game ever hangs, press %s to switch to Mono.",
            VkName(AerControl::GetMonoKey()));
        Help(hint);
    }
}

void DrawStereoAer()
{
    if (!ImGui::CollapsingHeader("AER (alternate-eye)", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    constexpr float kNeutralHalfIpd = 0.032f;
    float mm = AerControl::GetHalfEyeMeters() * 2000.0f;
    if (SliderReset("Stereo separation##aer", &mm, 20.0f, 180.0f, "%.0f mm", 69.0f))
    {
        AerControl::SetHalfEyeMeters(mm / 2000.0f);
        WriteFloat(L"HalfEyeMeters", AerControl::GetHalfEyeMeters());
    }
    const float half = AerControl::GetHalfEyeMeters();
    float scale = half > 0.0001f ? kNeutralHalfIpd / half : 1.0f;
    if (SliderReset("World scale##aer", &scale, 0.35f, 3.20f, "%.2fx", 1.0f))
    {
        AerControl::SetHalfEyeMeters(kNeutralHalfIpd / scale);
        WriteFloat(L"HalfEyeMeters", AerControl::GetHalfEyeMeters());
    }
    Help("Linked: more separation makes the world smaller.");
    float convergence = AerControl::GetConvergence();
    if (SliderReset("Convergence##aer", &convergence, -0.150f, 0.150f, "%+.4f", -0.0043f))
    {
        AerControl::SetConvergence(convergence);
        WriteFloat(L"Convergence", AerControl::GetConvergence());
    }
    bool swap = AerControl::GetSwapEyes();
    if (CheckReset("Invert depth (swap eyes)##aer", &swap, true))
    {
        AerControl::SetSwapEyes(swap);
        WriteBool(L"SwapEyes", swap);
    }
    bool eyeHold = AerControl::GetEyeHoldEnabled();
    if (CheckReset("Hold camera across eye pairs", &eyeHold, true))
    {
        AerControl::SetEyeHoldEnabled(eyeHold);
        WriteBool(L"EyeHold", eyeHold);
    }
    Help("Both eyes get the same camera, so a moving camera cannot swamp the separation.");
    bool simSync = AerControl::GetAerSimSync();
    if (CheckReset("Same game moment in both eyes", &simSync, true))
    {
        AerControl::SetAerSimSync(simSync);
        WriteBool(L"SimSync", simSync);
        LogMenuChange("Same game moment in both eyes", simSync ? 1.0f : 0.0f);
    }
    Help("Stops one eye running a frame ahead when driving or moving fast.");
    float fill = AerControl::GetFillFactor();
    if (SliderReset("Headset fill##aer", &fill, 0.40f, 1.80f, "%.2fx", 1.0f))
    {
        AerControl::SetFillFactor(fill);
        WriteFloat(L"FillFactor", AerControl::GetFillFactor());
    }
    if (fill > 1.02f)
        StatusLine(false, "Above 1.00 claims a wider view than was rendered, which flattens depth.");
}

void DrawGraphics()
{
    if (!ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    float sharpen = RetailXr::GetSharpen();
    if (SliderReset("Image sharpening", &sharpen, 0.0f, 1.0f, sharpen < 0.005f ? "off" : "%.2f", 0.25f))
    {
        RetailXr::SetSharpen(sharpen);
        WriteDisplayFloat(L"Sharpen", RetailXr::GetSharpen());
    }
    float textureBias = UiHook::GetTextureBias();
    if (SliderReset("Texture detail", &textureBias, 0.0f, -2.0f, textureBias > -0.005f ? "game default" : "%+.2f", -0.50f))
    {
        UiHook::SetTextureBias(textureBias);
        WriteDisplayFloat(L"TextureBias", UiHook::GetTextureBias());
    }
    Help("Sharper ground and wall textures; past about -1.0 they shimmer.");
    bool motionFix = MvFix::g_dlssFix.load();
    if (CheckReset("DLSS: correct camera motion", &motionFix, true))
    {
        MvFix::g_dlssFix.store(motionFix);
        WriteDisplayFloat(L"DlssMotionFix", motionFix ? 1.0f : 0.0f);
        RetailLogLine(motionFix ? "[MVFIX] ===== DLSS camera-motion correction ON =====" : "[MVFIX] ===== DLSS camera-motion correction OFF =====");
    }
    ImGui::TextDisabled("Game lens: %.0f degrees, the widest this engine accepts, in every camera mode.",
        AerControl::GetLiveFovDegrees());
}

void DrawTabVr()
{
    if (ImGui::CollapsingHeader("Keys", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int vrKey = AerControl::GetVrEnableKey();
        if (KeyRebindRow("Head tracking key", &vrKey, VK_F9, "vrEnableKey"))
        {
            AerControl::SetVrEnableKey(vrKey);
            WriteDisplayInt(L"VrEnableKey", vrKey);
        }
        Help("Press in the game to start VR and head tracking. Press again if tracking is ever lost.");
        int menuKey = g_menuKey;
        if (KeyRebindRow("Menu key", &menuKey, VK_INSERT, "menuKeyRow"))
        {
            g_menuKey = menuKey;
            g_insertWasDown = true;   // the bind press itself must not close the menu
            WriteDisplayInt(L"MenuKey", menuKey);
        }
        int recenterKey = g_recenterKey;
        if (KeyRebindRow("Recenter key", &recenterKey, 'R', "recenterKeyRow"))
        {
            g_recenterKey = recenterKey;
            g_recenterWasDown = true;   // the bind press itself must not fire
            WriteDisplayInt(L"RecenterKey", recenterKey);
        }
    }
    if (ImGui::CollapsingHeader("VR mode", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // 1 Stereo, 2 AER, 3 Mono (head tracked); 0 = no VR mode yet.
        const bool native = AerControl::IsNativeStereoEnabled();
        const bool mono = AerControl::IsMonoEnabled();
        const bool aer = AerControl::IsAerEnabled() && !mono;
        int mode = native ? 1 : (aer ? 2 : (mono ? 3 : 0));
        const int oldMode = mode;
        ImGui::RadioButton("Mono##mode", &mode, 3);
        ImGui::SameLine();
        ImGui::RadioButton("Stereo##mode", &mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("AER##mode", &mode, 2);
        if (mode != oldMode)
        {
            LogMenuChange("VR mode (1 stereo, 2 AER, 3 mono)", static_cast<float>(mode));
            // [MODETRACK2] each enable turns the other mode fully off first
            // and keeps head tracking running across the gap.
            if (mode == 1)
                AerControl::SetNativeStereoEnabled(true);
            else if (mode == 2)
                AerControl::SetAerEnabled(true);
            else
                AerControl::SetMonoEnabled(true);
            AerControl::SetPreferredVrMode(mode);   // [VRMODESAVE]
            WriteDisplayInt(L"VrMode", mode);
        }
        Help("Your pick is saved: the head tracking key starts it next time.");
    }
    if (AerControl::IsNativeStereoEnabled())
        DrawStereoNative();
    if (AerControl::IsAerEnabled() && !AerControl::IsMonoEnabled())
        DrawStereoAer();
    DrawGraphics();
}

void DrawTabTracking()
{
    if (ImGui::CollapsingHeader("Recenter", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Recenter view"))
            AerControl::Recenter();
        ImGui::SameLine();
        {
            char hint[64];
            std::snprintf(hint, sizeof(hint), "%s also recenters", VkName(g_recenterKey));
            Help(hint);
        }
    }
    if (ImGui::CollapsingHeader("Head tracking", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool sixDof = AerControl::GetSixDofEnabled();
        if (CheckReset("Positional tracking (lean, crouch)", &sixDof, true))
        {
            AerControl::SetSixDofEnabled(sixDof);
            WriteBool(L"SixDofEnabled", sixDof);
            AerControl::Recenter();
        }
        float positionScale = AerControl::GetSixDofPositionScale();
        if (SliderReset("Head movement scale", &positionScale, 0.0f, 2.0f, "%.2fx", 1.0f))
        {
            AerControl::SetSixDofPositionScale(positionScale);
            WriteFloat(L"SixDofPositionScale", AerControl::GetSixDofPositionScale());
        }
        float headTiming = AerControl::GetNativeHeadTiming();
        if (SliderReset("Head response timing", &headTiming, -1.0f, 1.0f, "%+.2f frames", 0.0f))
        {
            AerControl::SetNativeHeadTiming(headTiming);
            WriteFloat(L"NativeHeadTiming", AerControl::GetNativeHeadTiming());
        }
        Help("Adjust if the world trails or leads your head during turns.");
        bool decoupledPitch = AerControl::GetDecoupledPitch();
        if (CheckReset("Decoupled pitch", &decoupledPitch, false))
        {
            AerControl::SetDecoupledPitch(decoupledPitch);
            WriteBool(L"DecoupledPitch", decoupledPitch);
        }
        Help("Only your head tilts the view up and down.");
    }
}

void DrawTabFirstPerson()
{
    if (!ImGui::CollapsingHeader("First person", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    bool customFirstPerson = AerControl::GetCustomFirstPersonEnabled();
    if (ImGui::Checkbox("First-person mode (experimental)", &customFirstPerson))
    {
        AerControl::SetCustomFirstPersonEnabled(customFirstPerson);
        AerControl::Recenter();
    }
    int firstPersonKey = g_firstPersonKey;
    if (KeyRebindRow("First-person key", &firstPersonKey, 'K', "firstPersonKeyRow"))
    {
        g_firstPersonKey = firstPersonKey;
        g_customCameraWasDown = true;   // the bind press itself must not fire
        WriteDisplayInt(L"FirstPersonKey", firstPersonKey);
    }
    float cameraRight = 0.0f, cameraUp = 0.0f, cameraForward = 0.0f;
    AerControl::GetCustomFirstPersonOffsets(&cameraRight, &cameraUp, &cameraForward);
    bool changed = false;
    changed |= SliderReset("Camera left / right", &cameraRight, -0.50f, 0.50f, "%+.3f m", 0.0f);
    changed |= SliderReset("Camera height", &cameraUp, -0.75f, 0.75f, "%+.3f m", 0.0f);
    changed |= SliderReset("Camera forward / back", &cameraForward, -5.00f, 5.00f, "%+.3f m", 0.0f);
    if (changed)
    {
        AerControl::SetCustomFirstPersonOffsets(cameraRight, cameraUp, cameraForward);
        WriteFloat(L"FirstPersonRight", cameraRight);
        WriteFloat(L"FirstPersonUp", cameraUp);
        WriteFloat(L"FirstPersonForward", cameraForward);
    }
    Help("Leave the game in third person; these offsets place your own first-person view.");
}

void DrawTabHud()
{
    if (ImGui::CollapsingHeader("Headset HUD layer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool hudLayer = AerControl::GetHudLayerEnabled();
        if (CheckReset("Interface on its own layer", &hudLayer, true))
        {
            AerControl::SetHudLayerEnabled(hudLayer);
            WriteBool(L"HudLayer", hudLayer);
        }
        float hudDistance = 1.5f, hudWidth = 1.6f, hudHeight = 0.0f;
        AerControl::GetHudLayerParams(&hudDistance, &hudWidth, &hudHeight);
        bool hudChanged = false;
        ImGui::BeginDisabled(!hudLayer);
        hudChanged |= SliderReset("Distance", &hudDistance, 0.5f, 4.0f, "%.2f m", 1.5f);
        hudChanged |= SliderReset("Width", &hudWidth, 0.4f, 4.0f, "%.2f m", 1.6f);
        hudChanged |= SliderReset("Height offset", &hudHeight, -1.0f, 1.0f, "%+.2f m", 0.0f);
        ImGui::EndDisabled();
        if (hudChanged)
        {
            AerControl::SetHudLayerParams(hudDistance, hudWidth, hudHeight);
            AerControl::GetHudLayerParams(&hudDistance, &hudWidth, &hudHeight);
            WriteFloat(L"HudLayerDistance", hudDistance);
            WriteFloat(L"HudLayerWidth", hudWidth);
            WriteFloat(L"HudLayerHeight", hudHeight);
        }
        bool hudLens = AerControl::GetHudLensExact();
        if (CheckReset("World markers on the rendered lens", &hudLens, true))
        {
            AerControl::SetHudLensExact(hudLens);
            WriteBool(L"HudLensExact", hudLens);
        }
        Help("Lock-on, objectives and enemy bars sit on their targets; the rest stays on the panel.");
        float worldDistance = AerControl::GetHudWorldDistance();
        if (SliderReset("Marker layer distance", &worldDistance, 2.0f, 60.0f, "%.0f m", 20.0f))
        {
            AerControl::SetHudWorldDistance(worldDistance);
            WriteFloat(L"HudWorldDistance", worldDistance);
        }
        if (g_advanced)
        {
            bool baseCam = AerControl::GetHudWorldBaseCamera();
            if (CheckReset("Markers follow the game camera, not the head", &baseCam, false))
            {
                AerControl::SetHudWorldBaseCamera(baseCam);
                WriteBool(L"HudWorldBaseCamera", baseCam);
            }
        }
    }
    if (ImGui::CollapsingHeader("Interface fit (without the layer)"))
    {
        bool uiFit = AerControl::GetUiFitEnabled();
        if (CheckReset("Fit the interface to the headset", &uiFit, true))
        {
            AerControl::SetUiFitEnabled(uiFit);
            WriteBool(L"UiFit", uiFit);
        }
        float uiScaleX = 1.0f, uiScaleY = 1.0f, uiOffsetX = 0.0f, uiOffsetY = 0.0f;
        AerControl::GetUiFit(&uiScaleX, &uiScaleY, &uiOffsetX, &uiOffsetY);
        bool uiChanged = false;
        ImGui::BeginDisabled(!uiFit);
        uiChanged |= SliderReset("Width##fit", &uiScaleX, 0.40f, 1.10f, "%.3f", 0.85f);
        uiChanged |= SliderReset("Height##fit", &uiScaleY, 0.40f, 1.10f, "%.3f", 0.85f);
        uiChanged |= SliderReset("Left / right", &uiOffsetX, -0.25f, 0.25f, "%+.3f", 0.0f);
        uiChanged |= SliderReset("Up / down", &uiOffsetY, -0.25f, 0.25f, "%+.3f", 0.0f);
        ImGui::EndDisabled();
        if (uiChanged)
        {
            AerControl::SetUiFit(uiScaleX, uiScaleY, uiOffsetX, uiOffsetY);
            AerControl::GetUiFit(&uiScaleX, &uiScaleY, &uiOffsetX, &uiOffsetY);
            WriteFloat(L"UiFitScaleX", uiScaleX);
            WriteFloat(L"UiFitScaleY", uiScaleY);
            WriteFloat(L"UiFitOffsetX", uiOffsetX);
            WriteFloat(L"UiFitOffsetY", uiOffsetY);
        }
        Help("Used only when the HUD layer is off.");
    }
    if (ImGui::CollapsingHeader("Flat screens", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool flatMono = AerControl::GetFlatSceneMonoEnabled();
        if (CheckReset("Loading screens, menus and movies in mono", &flatMono, true))
        {
            AerControl::SetFlatSceneMonoEnabled(flatMono);
            WriteBool(L"FlatSceneMono", flatMono);
        }
        float videoFov = RetailXr::GetVideoScreenFov();
        if (SliderReset("Video / loading screen size", &videoFov, 30.0f, 110.0f, "%.0f deg wide", 70.0f))
        {
            RetailXr::SetVideoScreenFov(videoFov);
            wchar_t text[32]{};
            swprintf_s(text, L"%d", static_cast<int>(videoFov + 0.5f));
            if (g_iniPath[0])
                WritePrivateProfileStringW(L"Video", L"ScreenFovH", text, g_iniPath);
        }
        StatusLine(AerControl::GetSceneIsFlat(), AerControl::GetSceneIsFlat()
            ? "Right now: flat - one image to both eyes" : "Right now: 3D");
        bool dock = RetailXr::GetDockFlatScreens();
        if (CheckReset("Dock videos in front of you", &dock, false))
        {
            RetailXr::SetDockFlatScreens(dock);
            WriteBool(L"DockFlatScreens", dock);
        }
        Help("The screen stays where you were facing instead of following your head.");
    }
}

void DrawTabView()
{
    if (ImGui::CollapsingHeader("This menu", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float menuWidth = RetailXr::GetMenuWidth();
        if (SliderReset("Menu size", &menuWidth, 0.3f, 1.6f, "%.2f m wide", 0.75f))
        {
            RetailXr::SetMenuWidth(menuWidth);
            WriteDisplayFloat(L"MenuWidth", RetailXr::GetMenuWidth());
        }
        Help("Panel 1.2 m in front of you. Drag its top bar to move it.");
        if (ImGui::Button("Center menu"))
        {
            RetailXr::SetMenuOffset(0.0f, 0.0f);
            WriteDisplayFloat(L"MenuOffsetX", 0.0f);
            WriteDisplayFloat(L"MenuOffsetY", 0.0f);
        }
        if (ImGui::Checkbox("Show advanced settings", &g_advanced))
            WriteBool(L"Advanced", g_advanced);
        if (ImGui::Checkbox("Show the VR-key reminder on game start", &g_showVrHint))
            WriteBool(L"ShowVrHint", g_showVrHint);
    }
    if (g_advanced && ImGui::CollapsingHeader("Lens", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool lensFeed = AerControl::GetLensFeedEnabled();
        if (CheckReset("Match headset FOV to the rendered lens", &lensFeed, true))
        {
            AerControl::SetLensFeedEnabled(lensFeed);
            WriteBool(L"LensFeed", lensFeed);
        }
        Help("Cutscenes render a different lens than gameplay; claiming a fixed one flattens depth.");
    }
}

// [MENUINPUT] once per frame before ImGui::NewFrame.
void ApplyMenuInput(bool visible)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool focused = g_window && GetForegroundWindow() == g_window;
    const bool active = visible && focused;
    const bool wasActive = g_inputActive.exchange(active, std::memory_order_acq_rel);
    if (!active)
    {
        g_dragActive = false;
        io.AddMouseButtonEvent(0, false);
        io.AddMouseButtonEvent(1, false);
        return;
    }
    if (!wasActive)
    {
        // Free the real cursor and park it mid-screen, so its movement is
        // never cut short by a screen edge or the game's clip rectangle.
        if (g_realClipCursor) g_realClipCursor(nullptr); else ClipCursor(nullptr);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            const int cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
            const int cy = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
            if (g_realSetCursorPos) g_realSetCursorPos(cx, cy); else SetCursorPos(cx, cy);
        }
        InterlockedExchange(&g_llDX, 0);
        InterlockedExchange(&g_llDY, 0);
        InterlockedExchange(&g_llWheel, 0);
        InterlockedExchange(&g_llLBtn, 0);
        InterlockedExchange(&g_llRBtn, 0);
        GetCursorPos(&g_fallbackLast);
    }
    LONG dx = 0, dy = 0;
    bool left = false, right = false;
    if (g_mouseHook)
    {
        dx = InterlockedExchange(&g_llDX, 0);
        dy = InterlockedExchange(&g_llDY, 0);
        left = g_llLBtn != 0;
        right = g_llRBtn != 0;
        const LONG wheel = InterlockedExchange(&g_llWheel, 0);
        if (wheel)
            io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA));
    }
    else
    {
        POINT now{};
        GetCursorPos(&now);
        dx = now.x - g_fallbackLast.x;
        dy = now.y - g_fallbackLast.y;
        g_fallbackLast = now;
        left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    }
    // Keep the cursor on the menu: the panel only shows the menu's own rect.
    const bool haveRect = g_menuRect[2] > 16 && g_menuRect[3] > 16;
    const float minX = haveRect ? static_cast<float>(g_menuRect[0] + 6) : 0.0f;
    const float minY = haveRect ? static_cast<float>(g_menuRect[1] + 6) : 0.0f;
    const float maxX = haveRect ? static_cast<float>(g_menuRect[0] + g_menuRect[2] - 6) : io.DisplaySize.x;
    const float maxY = haveRect ? static_cast<float>(g_menuRect[1] + g_menuRect[3] - 6) : io.DisplaySize.y;
    if (!g_cursorPlaced)
    {
        g_cursorX = (minX + maxX) * 0.5f;
        g_cursorY = (minY + maxY) * 0.5f;
        g_cursorPlaced = true;
    }
    if (g_dragActive && left)
    {
        // Dragging the bar moves the panel in the room; the cursor rides with it.
        float offX = 0.0f, offY = 0.0f;
        RetailXr::GetMenuOffset(&offX, &offY);
        constexpr float kMetresPerCount = 0.0007f;
        RetailXr::SetMenuOffset(offX + dx * kMetresPerCount, offY - dy * kMetresPerCount);
    }
    else
    {
        g_cursorX = std::clamp(g_cursorX + static_cast<float>(dx), minX, maxX);
        g_cursorY = std::clamp(g_cursorY + static_cast<float>(dy), minY, maxY);
    }
    io.AddMousePosEvent(g_cursorX, g_cursorY);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
}

// [VRHINT] the load-time notification. No interaction (NoInputs): it must
// never steal the mouse/keyboard from the real menu or the game.
void DrawBanner()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##vrhint", nullptr, flags))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "Once you're in the game: press %s for VR.",
            VkName(AerControl::GetVrEnableKey()));
        ImGui::TextUnformatted(text);
        ImGui::TextUnformatted("Wait for a beep, head tracking takes a few seconds.");
    }
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    g_menuRect[0] = static_cast<int>(pos.x) - 4;
    g_menuRect[1] = static_cast<int>(pos.y) - 4;
    g_menuRect[2] = static_cast<int>(size.x) + 8;
    g_menuRect[3] = static_cast<int>(size.y) + 8;
    ImGui::End();
}

void DrawMenu()
{
    bool open = g_visible.load(std::memory_order_acquire);
    const ImGuiIO& io = ImGui::GetIO();
    // In the headset the menu is its own panel: fixed size, no chrome (the
    // fixed layout).  On the desktop it is an ordinary window.
    const bool panel = RetailXr::IsMenuLayerAvailable();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (panel)
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(940.0f, 740.0f), ImGuiCond_Always);
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(760.0f, 620.0f), ImGuiCond_Appearing);
    }
    if (ImGui::Begin("FFXV VR", &open, flags))
    {
        // [MENUDRAG] the panel has no title bar in the headset, so this bar
        // is the only way to grab it. Drag moves the panel itself in the
        // room (openxr_presenter.cpp's menu quad), not just content on its
        // own backing texture, which would do nothing visible.
        if (panel)
        {
            ImGui::Button("::: drag to reposition :::", ImVec2(-1.0f, 0.0f));
            const bool dragging = ImGui::IsItemActive();
            if (g_dragActive && !dragging)
            {
                float offX = 0.0f, offY = 0.0f;
                RetailXr::GetMenuOffset(&offX, &offY);
                WriteDisplayFloat(L"MenuOffsetX", offX);   // saved once, on release
                WriteDisplayFloat(L"MenuOffsetY", offY);
            }
            g_dragActive = dragging;
        }
        else
            g_dragActive = false;
        // Title row: name, mode, frame rate, close.
        const bool native = AerControl::IsNativeStereoEnabled();
        const bool aer = AerControl::IsAerEnabled();
        ImGui::TextUnformatted("FFXV VR");
        ImGui::SameLine();
        const bool monoMode = AerControl::IsMonoEnabled();
        ImGui::TextDisabled(native ? "Stereo" : (monoMode ? "Mono" : (aer ? "AER" : "Off")));
        const float gameFps = io.Framerate;
        const float perEyeHz = aer && !monoMode ? gameFps * 0.5f : gameFps;
        ImGui::SameLine();
        ImGui::TextColored(perEyeHz >= 55.0f ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.65f, 0.30f, 1.0f),
            "%.0f FPS", perEyeHz);
        if (panel)
        {
            ImGui::SameLine(ImGui::GetWindowWidth() - 170.0f);
            char closeLabel[48];
            std::snprintf(closeLabel, sizeof(closeLabel), "Close (%s)###close", VkName(g_menuKey));
            if (ImGui::SmallButton(closeLabel))
                open = false;
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("ffxvvr"))
        {
            if (ImGui::BeginTabItem("VR"))
            {
                ImGui::PushID("DrawTabVr");
                DrawTabVr();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tracking"))
            {
                ImGui::PushID("DrawTabTracking");
                DrawTabTracking();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("First Person"))
            {
                ImGui::PushID("DrawTabFirstPerson");
                DrawTabFirstPerson();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("HUD"))
            {
                ImGui::PushID("DrawTabHud");
                DrawTabHud();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("View"))
            {
                ImGui::PushID("DrawTabView");
                DrawTabView();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        {
            char keysLine[160];
            char recenterName[32], firstPersonName[32], menuName[32];
            std::snprintf(menuName, sizeof(menuName), "%s", VkName(g_menuKey));
            std::snprintf(recenterName, sizeof(recenterName), "%s", VkName(g_recenterKey));
            std::snprintf(firstPersonName, sizeof(firstPersonName), "%s", VkName(g_firstPersonKey));
            std::snprintf(keysLine, sizeof(keysLine),
                "%s: menu  |  %s: head tracking  |  %s: recenter  |  %s: first person",
                menuName, VkName(AerControl::GetVrEnableKey()), recenterName, firstPersonName);
            ImGui::TextDisabled("%s", keysLine);
        }
    }
    {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        g_menuRect[0] = static_cast<int>(pos.x) - 4;
        g_menuRect[1] = static_cast<int>(pos.y) - 4;
        g_menuRect[2] = static_cast<int>(size.x) + 8;
        g_menuRect[3] = static_cast<int>(size.y) + 8;
    }
    ImGui::End();
    if (!open)
        g_visible.store(false, std::memory_order_release);
}
} // namespace

namespace AerMenu
{
void OnPresent(IDXGISwapChain* swapChain)
{
    if (g_initializationFailed)
        return;
    if (!g_initialized && !Initialize(swapChain))
    {
        g_initializationFailed = true;
        RetailLogLine("[AERUI] initialization failed; VR continues without overlay");
        return;
    }

    const bool focused = !g_window || GetForegroundWindow() == g_window;
    const bool insertDown = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;   // [MENUKEY]
    if (insertDown && !g_insertWasDown && focused &&
        g_captureTarget.load(std::memory_order_acquire) == nullptr)
    {
        const bool nowVisible = !g_visible.load(std::memory_order_acquire);
        g_visible.store(nowVisible, std::memory_order_release);
        if (nowVisible)
            g_cursorPlaced = false;   // [MENUINPUT] cursor starts mid-menu each time it opens
    }
    g_insertWasDown = insertDown;

    // [MARK] Numpad 9: a marker line in the log at the moment it is pressed.
    const bool markDown = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    if (markDown && !g_markWasDown && focused && AerControl::GetDeveloperKeysEnabled())
    {
        static unsigned s_marks = 0;
        char line[96];
        std::snprintf(line, sizeof(line), "[MARK] ===== USER MARK %u (Numpad 9) =====", ++s_marks);
        RetailLogLine(line);
        Beep(1600, 80);
        DlssUpscaler::RequestPairBurst(40);   // [PAIRBURST]
        SnapBegin();                          // [SNAP]
        AerControl::RequestLensSnapshot();    // [LENSWATCH]
    }
    g_markWasDown = markDown;
    SnapOnPresent(swapChain);   // [SNAP]
    const bool imageHoldDown = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    if (imageHoldDown && !g_imageHoldWasDown && focused)
        RetailXr::SetNativeImageHold(!RetailXr::GetNativeImageHold());
    g_imageHoldWasDown = imageHoldDown;
    const bool recenterDown = (GetAsyncKeyState(g_recenterKey) & 0x8000) != 0;   // [KEYBIND]
    const bool textInputActive = g_visible.load(std::memory_order_acquire) &&
        ImGui::GetIO().WantTextInput;
    if (recenterDown && !g_recenterWasDown && focused && !textInputActive)
    {
        AerControl::Recenter();
        RetailLogLine("[AERUI] R -> headset recentered");
    }
    g_recenterWasDown = recenterDown;

    const bool customCameraDown = (GetAsyncKeyState(g_firstPersonKey) & 0x8000) != 0;   // [KEYBIND]
    if (customCameraDown && !g_customCameraWasDown && focused && !textInputActive)
    {
        const bool enabled = !AerControl::GetCustomFirstPersonEnabled();
        AerControl::SetCustomFirstPersonEnabled(enabled);
        RetailLogLine(enabled
            ? "[AERUI] K -> custom camera ON"
            : "[AERUI] K -> default third-person camera");
    }
    g_customCameraWasDown = customCameraDown;

    const bool visible = g_visible.load(std::memory_order_acquire);
    const auto xrReadyTick = RetailXr::GetXrReadyTick();
    const bool showBanner = !visible && g_showVrHint && xrReadyTick != 0 &&
        (GetTickCount64() - xrReadyTick) < 5000;
    if ((!visible && !showBanner) || !RefreshRenderTarget(swapChain))
    {
        RetailXr::SetMenuLayer(g_menuTexture, false, 0, 0, 0, 0);
        return;
    }

    // The OS cursor is monitor-only. Bake ImGui's software cursor into the
    // backbuffer so it survives the pre-OpenXR headset capture.
    ImGui::GetIO().MouseDrawCursor = visible;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // [MENUSIZE] the backend sizes the menu from the WINDOW; below the monitor's
    // resolution the borderless window stays monitor-sized while the game's
    // image (and the menu texture) is smaller, so the centred menu landed
    // partly outside the image (cut off at 1080p/1440p).  Lay out at the size
    // of the image actually drawn into.
    if (g_backBuffer)
    {
        D3D11_TEXTURE2D_DESC desc{};
        g_backBuffer->GetDesc(&desc);
        if (desc.Width && desc.Height)
        {
            ImGuiIO& sizeIo = ImGui::GetIO();
            sizeIo.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
            sizeIo.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
    }
    ApplyMenuInput(visible);   // [MENUINPUT]
    ImGui::NewFrame();
    if (visible)
        DrawMenu();
    else
        DrawBanner();   // [VRHINT]
    ImGui::Render();

    ID3D11RenderTargetView* previousTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* previousDepth{};
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        previousTargets, &previousDepth);
    UINT previousViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    g_context->RSGetViewports(&previousViewportCount, previousViewports);
    // [MENUQUAD] In the headset the menu goes on its own panel (fixed
    // layout); it is then not baked into the game image, so it cannot be cut
    // by the stereo seam or shrunk with the interface layer.  Without a
    // headset consumer it is drawn on the desktop backbuffer as before.
    bool onPanel = false;
    if (RetailXr::IsMenuLayerAvailable() && EnsureMenuTexture(swapChain))
    {
        const float clear[4]{};
        g_context->ClearRenderTargetView(g_menuTarget, clear);
        g_context->OMSetRenderTargets(1, &g_menuTarget, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        RetailXr::SetMenuLayer(g_menuTexture, true, g_menuRect[0], g_menuRect[1], g_menuRect[2], g_menuRect[3]);
        onPanel = true;
    }
    if (!onPanel)
    {
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        previousTargets, previousDepth);
    for (ID3D11RenderTargetView* target : previousTargets)
    {
        if (target)
            target->Release();
    }
    if (previousDepth)
        previousDepth->Release();
    if (previousViewportCount > 0)
        g_context->RSSetViewports(previousViewportCount, previousViewports);
}
} // namespace AerMenu
