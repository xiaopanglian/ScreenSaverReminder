#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdint>
#include <cwctype>
#include <algorithm>
#include <iterator>
#include <vector>
#include <string>
#include <string_view>

#include "resource.h"

static constexpr UINT WMAPP_TRAY = WM_APP + 1;
static constexpr UINT WMAPP_ACTIVITY = WM_APP + 2;

static constexpr UINT_PTR TIMER_INTERVAL = 1;
static constexpr UINT_PTR TIMER_OVERLAY_ANIM = 2;
static constexpr UINT_PTR TIMER_OVERLAY_CLOCK = 3;

static constexpr int TEXT_MAX_LEN = 500;

enum class OverlayState : int
{
    Hidden = 0,
    FadingIn = 1,
    WaitingInput = 2,
    FadingOut = 3,
};

struct AppConfig
{
    int intervalMinutes = 15;
    int opacityPercent = 60;
    int fadeSeconds = 5;
    COLORREF bgColor = RGB(0, 128, 64); // #008040
    bool autoStart = false;
    std::wstring text = L"抬眼望远处，给目光放个假。";
};

static HINSTANCE g_hInstance = nullptr;
static HWND g_hwndMain = nullptr;
static HWND g_hwndSettings = nullptr;
static std::vector<HWND> g_overlayWindows;
static HBRUSH g_settingsBgBrush = nullptr;

static NOTIFYICONDATAW g_nid{};
static HMENU g_trayMenu = nullptr;

static HHOOK g_hHookKeyboard = nullptr;
static HHOOK g_hHookMouse = nullptr;

static std::atomic<OverlayState> g_overlayState{OverlayState::Hidden};
static std::atomic<long> g_activityLatch{0};
static std::atomic<bool> g_exiting{false};

static ULONGLONG g_fadeStartTick = 0;
static BYTE g_targetAlpha = 153;
static BYTE g_currentAlpha = 0;

static AppConfig g_config{};
static AppConfig g_overlayConfig{};

static void Overlay_ShowWithConfig(HWND hwnd, const AppConfig& cfg);
static bool Settings_TryBuildCandidateFromControls(HWND hwndDlg, AppConfig& candidate, std::wstring& error);
static bool AutoStart_Apply(bool enabled, std::wstring& error);

static std::wstring GetAppDataFolder()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) || path == nullptr)
    {
        return L".";
    }

    std::wstring folder(path);
    CoTaskMemFree(path);
    folder += L"\\ScreenSaverReminderCPP";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

static std::wstring GetConfigIniPath()
{
    return GetAppDataFolder() + L"\\config.ini";
}

static std::wstring GetTextPath()
{
    return GetAppDataFolder() + L"\\text.txt";
}

static std::string WideToUtf8(const std::wstring& input)
{
    if (input.empty())
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), (int)input.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string out;
    out.resize((size_t)size);
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), (int)input.size(), out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& input)
{
    if (input.empty())
    {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), (int)input.size(), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }

    std::wstring out;
    out.resize((size_t)size);
    MultiByteToWideChar(CP_UTF8, 0, input.data(), (int)input.size(), out.data(), size);
    return out;
}

static bool ReadFileUtf8(const std::wstring& path, std::wstring& contentOut)
{
    contentOut.clear();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > (1024 * 1024))
    {
        CloseHandle(hFile);
        return false;
    }

    std::string buffer;
    buffer.resize((size_t)size.QuadPart);

    DWORD read = 0;
    const BOOL ok = ReadFile(hFile, buffer.data(), (DWORD)buffer.size(), &read, nullptr);
    CloseHandle(hFile);
    if (!ok)
    {
        return false;
    }
    buffer.resize(read);

    contentOut = Utf8ToWide(buffer);
    return true;
}

static bool WriteFileUtf8(const std::wstring& path, const std::wstring& content)
{
    const auto bytes = WideToUtf8(content);
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(hFile, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(hFile);
    return ok == TRUE;
}

static std::wstring Trim(std::wstring_view s)
{
    size_t start = 0;
    while (start < s.size() && iswspace(s[start])) start++;
    size_t end = s.size();
    while (end > start && iswspace(s[end - 1])) end--;
    return std::wstring(s.substr(start, end - start));
}

static bool TryParseHexColor(const std::wstring& input, COLORREF& colorOut)
{
    auto s = Trim(input);
    if (!s.empty() && s[0] == L'#')
    {
        s.erase(0, 1);
    }
    if (s.size() != 6)
    {
        return false;
    }
    auto hexVal = [](wchar_t ch) -> int
    {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
        if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
        return -1;
    };
    int r1 = hexVal(s[0]), r2 = hexVal(s[1]);
    int g1 = hexVal(s[2]), g2 = hexVal(s[3]);
    int b1 = hexVal(s[4]), b2 = hexVal(s[5]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0)
    {
        return false;
    }
    int r = (r1 << 4) | r2;
    int g = (g1 << 4) | g2;
    int b = (b1 << 4) | b2;
    colorOut = RGB(r, g, b);
    return true;
}

static std::wstring ColorToHex(COLORREF c)
{
    wchar_t buf[16]{};
    wsprintfW(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

static void NormalizeConfig(AppConfig& cfg)
{
    if (cfg.intervalMinutes < 1) cfg.intervalMinutes = 1;
    if (cfg.fadeSeconds < 1) cfg.fadeSeconds = 1;
    if (cfg.opacityPercent < 0) cfg.opacityPercent = 0;
    if (cfg.opacityPercent > 100) cfg.opacityPercent = 100;
    if (cfg.text.size() > TEXT_MAX_LEN) cfg.text.resize(TEXT_MAX_LEN);
}

static void LoadConfig(AppConfig& cfg)
{
    cfg = AppConfig{};
    const auto iniPath = GetConfigIniPath();

    wchar_t buf[2048]{};

    cfg.intervalMinutes = GetPrivateProfileIntW(L"General", L"IntervalMinutes", cfg.intervalMinutes, iniPath.c_str());
    cfg.opacityPercent = GetPrivateProfileIntW(L"General", L"OpacityPercent", cfg.opacityPercent, iniPath.c_str());
    cfg.fadeSeconds = GetPrivateProfileIntW(L"General", L"FadeSeconds", cfg.fadeSeconds, iniPath.c_str());
    cfg.autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", cfg.autoStart ? 1 : 0, iniPath.c_str()) != 0;

    GetPrivateProfileStringW(L"General", L"BgColorHex", L"#000000", buf, (DWORD)std::size(buf), iniPath.c_str());
    COLORREF color{};
    if (TryParseHexColor(buf, color))
    {
        cfg.bgColor = color;
    }

    std::wstring text;
    if (ReadFileUtf8(GetTextPath(), text))
    {
        cfg.text = text;
    }
    else
    {
        GetPrivateProfileStringW(L"General", L"Text", L"", buf, (DWORD)std::size(buf), iniPath.c_str());
        cfg.text = buf;
    }

    NormalizeConfig(cfg);
}

static void SaveConfig(const AppConfig& cfg)
{
    const auto iniPath = GetConfigIniPath();

    wchar_t tmp[64]{};

    wsprintfW(tmp, L"%d", cfg.intervalMinutes);
    WritePrivateProfileStringW(L"General", L"IntervalMinutes", tmp, iniPath.c_str());

    wsprintfW(tmp, L"%d", cfg.opacityPercent);
    WritePrivateProfileStringW(L"General", L"OpacityPercent", tmp, iniPath.c_str());

    wsprintfW(tmp, L"%d", cfg.fadeSeconds);
    WritePrivateProfileStringW(L"General", L"FadeSeconds", tmp, iniPath.c_str());

    const auto hex = ColorToHex(cfg.bgColor);
    WritePrivateProfileStringW(L"General", L"BgColorHex", hex.c_str(), iniPath.c_str());

    WritePrivateProfileStringW(L"General", L"AutoStart", cfg.autoStart ? L"1" : L"0", iniPath.c_str());
    WriteFileUtf8(GetTextPath(), cfg.text);
}

static void Tray_ShowMenu(HWND hwnd)
{
    if (!g_trayMenu)
    {
        return;
    }

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

static void Tray_Create(HWND hwnd)
{
    g_trayMenu = CreatePopupMenu();
    AppendMenuW(g_trayMenu, MF_STRING, IDM_TRAY_OPEN_SETTINGS, L"打开设置");
    AppendMenuW(g_trayMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_trayMenu, MF_STRING, IDM_TRAY_EXIT, L"退出");

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WMAPP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_hInstance, L"favicon", IMAGE_ICON, 16, 16, 0);
    if (!g_nid.hIcon)
    {
        g_nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_FAVICON));
    }
    lstrcpynW(g_nid.szTip, L"屏保提醒工具", (int)std::size(g_nid.szTip));

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void Tray_Destroy()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon)
    {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
    if (g_trayMenu)
    {
        DestroyMenu(g_trayMenu);
        g_trayMenu = nullptr;
    }
}

static void Scheduler_Start(HWND hwnd)
{
    KillTimer(hwnd, TIMER_INTERVAL);
    const UINT elapseMs = (UINT)g_config.intervalMinutes * 60u * 1000u;
    SetTimer(hwnd, TIMER_INTERVAL, elapseMs, nullptr);
}

static void Scheduler_Stop(HWND hwnd)
{
    KillTimer(hwnd, TIMER_INTERVAL);
}

static void Overlay_SetAlpha(HWND hwnd, BYTE alpha)
{
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
}

static void Overlay_Invalidate(HWND hwnd)
{
    InvalidateRect(hwnd, nullptr, FALSE);
}

static bool Overlay_IsVisible()
{
    return !g_overlayWindows.empty();
}

static void Overlay_SetAlphaAll(BYTE alpha)
{
    for (HWND w : g_overlayWindows)
    {
        if (w)
        {
            Overlay_SetAlpha(w, alpha);
        }
    }
}

static void Overlay_InvalidateAll()
{
    for (HWND w : g_overlayWindows)
    {
        if (w)
        {
            Overlay_Invalidate(w);
        }
    }
}

static BOOL CALLBACK EnumMonitorsProc(HMONITOR, HDC, LPRECT rc, LPARAM lParam)
{
    auto* rects = reinterpret_cast<std::vector<RECT>*>(lParam);
    rects->push_back(*rc);
    return TRUE;
}

static std::vector<RECT> GetMonitorRects()
{
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&rects));
    return rects;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        if (g_overlayState.load() == OverlayState::WaitingInput)
        {
            if (g_activityLatch.exchange(1) == 0)
            {
                PostMessageW(g_hwndMain, WMAPP_ACTIVITY, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        if (g_overlayState.load() == OverlayState::WaitingInput)
        {
            if (g_activityLatch.exchange(1) == 0)
            {
                PostMessageW(g_hwndMain, WMAPP_ACTIVITY, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static void InputMonitor_Start()
{
    if (g_hHookKeyboard || g_hHookMouse)
    {
        return;
    }
    g_activityLatch.store(0);
    g_hHookKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInstance, 0);
    g_hHookMouse = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_hInstance, 0);
}

static void InputMonitor_Stop()
{
    if (g_hHookKeyboard)
    {
        UnhookWindowsHookEx(g_hHookKeyboard);
        g_hHookKeyboard = nullptr;
    }
    if (g_hHookMouse)
    {
        UnhookWindowsHookEx(g_hHookMouse);
        g_hHookMouse = nullptr;
    }
    g_activityLatch.store(0);
}

static void Overlay_Show(HWND hwnd)
{
    Overlay_ShowWithConfig(hwnd, g_config);
}

static void Overlay_ShowWithConfig(HWND hwnd, const AppConfig& cfg)
{
    if (Overlay_IsVisible())
    {
        return;
    }

    g_overlayConfig = cfg;

    const auto rects = GetMonitorRects();
    if (rects.empty())
    {
        return;
    }

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    const DWORD style = WS_POPUP;

    for (const auto& r : rects)
    {
        HWND w = CreateWindowExW(
            exStyle,
            L"SSR_OVERLAY",
            L"",
            style,
            r.left, r.top,
            r.right - r.left, r.bottom - r.top,
            nullptr, nullptr, g_hInstance, nullptr
        );

        if (!w)
        {
            for (HWND created : g_overlayWindows)
            {
                if (created)
                {
                    DestroyWindow(created);
                }
            }
            g_overlayWindows.clear();
            return;
        }

        g_overlayWindows.push_back(w);
    }

    g_targetAlpha = (BYTE)((g_overlayConfig.opacityPercent * 255) / 100);
    g_currentAlpha = 0;

    Overlay_SetAlphaAll(0);
    for (HWND w : g_overlayWindows)
    {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    g_overlayState.store(OverlayState::FadingIn);
    g_fadeStartTick = GetTickCount64();

    InputMonitor_Start();

    SetTimer(hwnd, TIMER_OVERLAY_CLOCK, 1000, nullptr);
    SetTimer(hwnd, TIMER_OVERLAY_ANIM, 15, nullptr);
    Overlay_InvalidateAll();
}

static void Overlay_BeginFadeOut(HWND hwnd)
{
    if (!Overlay_IsVisible())
    {
        return;
    }
    if (g_overlayState.load() == OverlayState::FadingOut)
    {
        return;
    }
    g_overlayState.store(OverlayState::FadingOut);
    g_fadeStartTick = GetTickCount64();
    SetTimer(hwnd, TIMER_OVERLAY_ANIM, 15, nullptr);
}

static void Overlay_DestroyAll(HWND hwnd, bool restartScheduler)
{
    if (!Overlay_IsVisible())
    {
        return;
    }

    KillTimer(hwnd, TIMER_OVERLAY_ANIM);
    KillTimer(hwnd, TIMER_OVERLAY_CLOCK);

    for (HWND w : g_overlayWindows)
    {
        if (w)
        {
            DestroyWindow(w);
        }
    }
    g_overlayWindows.clear();

    g_overlayState.store(OverlayState::Hidden);
    InputMonitor_Stop();

    if (restartScheduler)
    {
        Scheduler_Start(hwnd);
    }
}

static void Overlay_Hide(HWND hwnd)
{
    Overlay_DestroyAll(hwnd, true);
}

static void Overlay_TickAnim(HWND hwnd)
{
    if (!Overlay_IsVisible())
    {
        KillTimer(hwnd, TIMER_OVERLAY_ANIM);
        return;
    }

    const auto state = g_overlayState.load();
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsedMs = now - g_fadeStartTick;
    const ULONGLONG durationMs = (ULONGLONG)g_overlayConfig.fadeSeconds * 1000ull;

    if (durationMs == 0)
    {
        return;
    }

    if (state == OverlayState::FadingIn)
    {
        if (elapsedMs >= durationMs)
        {
            g_currentAlpha = g_targetAlpha;
            Overlay_SetAlphaAll(g_currentAlpha);
            g_overlayState.store(OverlayState::WaitingInput);
            g_activityLatch.store(0);
            KillTimer(hwnd, TIMER_OVERLAY_ANIM);
            return;
        }

        const double t = (double)elapsedMs / (double)durationMs;
        const int alpha = (int)(t * (double)g_targetAlpha);
        g_currentAlpha = (BYTE)std::clamp(alpha, 0, 255);
        Overlay_SetAlphaAll(g_currentAlpha);
        return;
    }

    if (state == OverlayState::FadingOut)
    {
        if (elapsedMs >= durationMs)
        {
            g_currentAlpha = 0;
            Overlay_SetAlphaAll(0);
            Overlay_Hide(hwnd);
            return;
        }

        const double t = (double)elapsedMs / (double)durationMs;
        const int alpha = (int)((1.0 - t) * (double)g_targetAlpha);
        g_currentAlpha = (BYTE)std::clamp(alpha, 0, 255);
        Overlay_SetAlphaAll(g_currentAlpha);
        return;
    }
}

static HFONT CreateUIFont(int pointSize, int dpi, bool bold)
{
    const int heightPx = -MulDiv(pointSize, dpi, 72);
    return CreateFontW(heightPx, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void Overlay_Paint(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int dpi = GetDpiForWindow(hwnd);
    HFONT fontTime = CreateUIFont(72, dpi, true);
    HFONT fontText = CreateUIFont(36, dpi, false);

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    HBRUSH brush = CreateSolidBrush(g_overlayConfig.bgColor);
    FillRect(memDc, &rc, brush);
    DeleteObject(brush);

    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, RGB(255, 255, 255));

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timeBuf[32]{};
    wsprintfW(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    const int marginX = MulDiv(80, dpi, 96);
    const int gap = MulDiv(18, dpi, 96);
    const int availWidth = std::max(1, width - (marginX * 2));

    SelectObject(memDc, fontTime);
    RECT timeCalc{ 0, 0, availWidth, 0 };
    DrawTextW(memDc, timeBuf, -1, &timeCalc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int timeH = timeCalc.bottom - timeCalc.top;

    int textH = 0;
    if (!g_overlayConfig.text.empty())
    {
        SelectObject(memDc, fontText);
        RECT textCalc{ 0, 0, availWidth, 0 };
        DrawTextW(memDc, g_overlayConfig.text.c_str(), (int)g_overlayConfig.text.size(), &textCalc,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        textH = textCalc.bottom - textCalc.top;
    }

    const int combinedH = timeH + (textH > 0 ? (gap + textH) : 0);
    int startY = (height - combinedH) / 2;
    if (startY < 0) startY = 0;

    RECT rcTime{ marginX, startY, width - marginX, startY + timeH };
    SelectObject(memDc, fontTime);
    DrawTextW(memDc, timeBuf, -1, &rcTime, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (!g_overlayConfig.text.empty())
    {
        RECT rcText{ marginX, rcTime.bottom + gap, width - marginX, rcTime.bottom + gap + textH };
        SelectObject(memDc, fontText);
        DrawTextW(memDc, g_overlayConfig.text.c_str(), (int)g_overlayConfig.text.size(), &rcText,
            DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }

    DeleteObject(fontTime);
    DeleteObject(fontText);

    BitBlt(hdc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);

    EndPaint(hwnd, &ps);
}

static void Settings_Show(HWND hwndOwner);

static std::wstring QuoteForCommandLine(const std::wstring& path)
{
    if (path.empty())
    {
        return L"";
    }
    if (path.find(L' ') != std::wstring::npos || path.find(L'\t') != std::wstring::npos)
    {
        return L"\"" + path + L"\"";
    }
    return path;
}

static bool AutoStart_Apply(bool enabled, std::wstring& error)
{
    error.clear();

    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_SET_VALUE,
        &key
    );

    if (status != ERROR_SUCCESS || key == nullptr)
    {
        error = L"无法打开开机启动注册表项（HKCU Run）。";
        return false;
    }

    const wchar_t* valueName = L"ScreenSaverReminderCPP";

    if (!enabled)
    {
        const auto delStatus = RegDeleteValueW(key, valueName);
        RegCloseKey(key);
        if (delStatus == ERROR_SUCCESS || delStatus == ERROR_FILE_NOT_FOUND)
        {
            return true;
        }
        error = L"关闭开机自启失败。";
        return false;
    }

    wchar_t exePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, (DWORD)std::size(exePath));
    if (len == 0 || len >= std::size(exePath))
    {
        RegCloseKey(key);
        error = L"获取程序路径失败。";
        return false;
    }

    const auto command = QuoteForCommandLine(exePath);
    const DWORD bytes = (DWORD)((command.size() + 1) * sizeof(wchar_t));

    const auto setStatus = RegSetValueExW(
        key,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        bytes
    );

    RegCloseKey(key);

    if (setStatus != ERROR_SUCCESS)
    {
        error = L"开启开机自启失败。";
        return false;
    }

    return true;
}

static void App_Exit(HWND hwnd)
{
    if (g_exiting.exchange(true))
    {
        return;
    }
    Scheduler_Stop(hwnd);
    InputMonitor_Stop();
    Overlay_DestroyAll(hwnd, false);
    if (g_hwndSettings)
    {
        DestroyWindow(g_hwndSettings);
        g_hwndSettings = nullptr;
    }
    Tray_Destroy();
    PostQuitMessage(0);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Overlay_Paint(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void SetEditInt(HWND hEdit, int value)
{
    wchar_t buf[32]{};
    wsprintfW(buf, L"%d", value);
    SetWindowTextW(hEdit, buf);
}

static int GetEditInt(HWND hEdit)
{
    wchar_t buf[64]{};
    GetWindowTextW(hEdit, buf, (int)std::size(buf));
    return _wtoi(buf);
}

static void Settings_UpdateCount(HWND hwndDlg)
{
    HWND hText = GetDlgItem(hwndDlg, IDC_TEXT_EDIT);
    HWND hCount = GetDlgItem(hwndDlg, IDC_TEXT_COUNT);
    const int len = GetWindowTextLengthW(hText);
    wchar_t buf[64]{};
    wsprintfW(buf, L"%d/%d", len, TEXT_MAX_LEN);
    SetWindowTextW(hCount, buf);
}

static void Settings_LoadToControls(HWND hwndDlg)
{
    SetEditInt(GetDlgItem(hwndDlg, IDC_INTERVAL_EDIT), g_config.intervalMinutes);
    SetEditInt(GetDlgItem(hwndDlg, IDC_FADE_EDIT), g_config.fadeSeconds);
    SetEditInt(GetDlgItem(hwndDlg, IDC_OPACITY_EDIT), g_config.opacityPercent);
    SendMessageW(GetDlgItem(hwndDlg, IDC_OPACITY_TRACK), TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(GetDlgItem(hwndDlg, IDC_OPACITY_TRACK), TBM_SETPOS, TRUE, g_config.opacityPercent);

    SetWindowTextW(GetDlgItem(hwndDlg, IDC_COLOR_EDIT), ColorToHex(g_config.bgColor).c_str());
    SetWindowTextW(GetDlgItem(hwndDlg, IDC_TEXT_EDIT), g_config.text.c_str());
    SendMessageW(GetDlgItem(hwndDlg, IDC_AUTOSTART_CHECK), BM_SETCHECK, g_config.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    Settings_UpdateCount(hwndDlg);
}

static bool Settings_SaveFromControls(HWND hwndDlg, std::wstring& error)
{
    AppConfig candidate{};
    if (!Settings_TryBuildCandidateFromControls(hwndDlg, candidate, error))
    {
        return false;
    }

    std::wstring autoStartErr;
    if (!AutoStart_Apply(candidate.autoStart, autoStartErr))
    {
        error = autoStartErr;
        return false;
    }

    g_config = candidate;
    SaveConfig(g_config);

    Scheduler_Start(g_hwndMain);
    return true;
}

static bool Settings_TryBuildCandidateFromControls(HWND hwndDlg, AppConfig& candidate, std::wstring& error)
{
    candidate = g_config;

    candidate.intervalMinutes = GetEditInt(GetDlgItem(hwndDlg, IDC_INTERVAL_EDIT));
    candidate.fadeSeconds = GetEditInt(GetDlgItem(hwndDlg, IDC_FADE_EDIT));
    candidate.opacityPercent = GetEditInt(GetDlgItem(hwndDlg, IDC_OPACITY_EDIT));

    if (candidate.intervalMinutes < 1)
    {
        error = L"间隔（分钟）最小为 1。";
        return false;
    }
    if (candidate.fadeSeconds < 1)
    {
        error = L"淡入/淡出（秒）最小为 1。";
        return false;
    }
    if (candidate.opacityPercent < 0 || candidate.opacityPercent > 100)
    {
        error = L"透明度必须在 0-100 之间。";
        return false;
    }

    wchar_t colorBuf[32]{};
    GetWindowTextW(GetDlgItem(hwndDlg, IDC_COLOR_EDIT), colorBuf, (int)std::size(colorBuf));
    COLORREF color{};
    if (!TryParseHexColor(colorBuf, color))
    {
        error = L"背景颜色格式不正确，请输入类似 #008040 的 HEX。";
        return false;
    }
    candidate.bgColor = color;

    const int textLen = GetWindowTextLengthW(GetDlgItem(hwndDlg, IDC_TEXT_EDIT));
    std::wstring text;
    text.resize((size_t)textLen);
    GetWindowTextW(GetDlgItem(hwndDlg, IDC_TEXT_EDIT), text.data(), textLen + 1);
    if (text.size() > TEXT_MAX_LEN)
    {
        text.resize(TEXT_MAX_LEN);
    }
    candidate.text = std::move(text);

    candidate.autoStart = SendMessageW(GetDlgItem(hwndDlg, IDC_AUTOSTART_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;

    NormalizeConfig(candidate);
    return true;
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CTLCOLORDLG:
        return (INT_PTR)(g_settingsBgBrush ? g_settingsBgBrush : GetStockObject(WHITE_BRUSH));
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, RGB(0, 0, 0));
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(250, 250, 250));
        SetTextColor(hdc, RGB(0, 0, 0));
        return (INT_PTR)(g_settingsBgBrush ? g_settingsBgBrush : GetStockObject(WHITE_BRUSH));
    }
    case WM_CREATE:
    {
        const int dpi = GetDpiForWindow(hwnd);
        auto D = [dpi](int x) { return MulDiv(x, dpi, 96); };

        CreateWindowExW(0, L"STATIC", L"间隔（分钟）", WS_CHILD | WS_VISIBLE, D(14), D(14), D(130), D(22), hwnd, nullptr, g_hInstance, nullptr);
        HWND hInterval = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, D(150), D(12), D(120), D(26), hwnd, (HMENU)IDC_INTERVAL_EDIT, g_hInstance, nullptr);
        SendMessageW(hInterval, EM_SETLIMITTEXT, 4, 0);

        CreateWindowExW(0, L"STATIC", L"背景颜色（HEX）", WS_CHILD | WS_VISIBLE, D(14), D(48), D(130), D(22), hwnd, nullptr, g_hInstance, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE, D(150), D(46), D(120), D(26), hwnd, (HMENU)IDC_COLOR_EDIT, g_hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"选择", WS_CHILD | WS_VISIBLE, D(280), D(46), D(70), D(26), hwnd, (HMENU)IDC_COLOR_PICK, g_hInstance, nullptr);

        CreateWindowExW(0, L"STATIC", L"透明度（0-100）", WS_CHILD | WS_VISIBLE, D(14), D(82), D(130), D(22), hwnd, nullptr, g_hInstance, nullptr);
        HWND hTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, D(150), D(78), D(180), D(30), hwnd, (HMENU)IDC_OPACITY_TRACK, g_hInstance, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, D(340), D(78), D(60), D(26), hwnd, (HMENU)IDC_OPACITY_EDIT, g_hInstance, nullptr);
        SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_EDIT), EM_SETLIMITTEXT, 3, 0);

        CreateWindowExW(0, L"STATIC", L"淡入/淡出（秒）", WS_CHILD | WS_VISIBLE, D(14), D(116), D(130), D(22), hwnd, nullptr, g_hInstance, nullptr);
        HWND hFade = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, D(150), D(114), D(120), D(26), hwnd, (HMENU)IDC_FADE_EDIT, g_hInstance, nullptr);
        SendMessageW(hFade, EM_SETLIMITTEXT, 3, 0);

        CreateWindowExW(0, L"STATIC", L"显示文字（可选，最多500字）", WS_CHILD | WS_VISIBLE, D(14), D(152), D(250), D(22), hwnd, nullptr, g_hInstance, nullptr);
        HWND hText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            D(14), D(176), D(386), D(140), hwnd, (HMENU)IDC_TEXT_EDIT, g_hInstance, nullptr);
        SendMessageW(hText, EM_SETLIMITTEXT, TEXT_MAX_LEN, 0);

        CreateWindowExW(0, L"STATIC", L"0/500", WS_CHILD | WS_VISIBLE | SS_RIGHT, D(300), D(322), D(100), D(22), hwnd, (HMENU)IDC_TEXT_COUNT, g_hInstance, nullptr);

        CreateWindowExW(0, L"BUTTON", L"开机自启", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, D(14), D(350), D(130), D(30), hwnd, (HMENU)IDC_AUTOSTART_CHECK, g_hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"预览", WS_CHILD | WS_VISIBLE, D(170), D(350), D(110), D(30), hwnd, (HMENU)IDC_PREVIEW, g_hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE, D(290), D(350), D(110), D(30), hwnd, (HMENU)IDC_SAVE, g_hInstance, nullptr);

        Settings_LoadToControls(hwnd);
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == IDC_COLOR_PICK && code == BN_CLICKED)
        {
            CHOOSECOLORW cc{};
            COLORREF custom[16]{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.rgbResult = g_config.bgColor;
            cc.lpCustColors = custom;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
            {
                g_config.bgColor = cc.rgbResult;
                SetWindowTextW(GetDlgItem(hwnd, IDC_COLOR_EDIT), ColorToHex(g_config.bgColor).c_str());
            }
            return 0;
        }

        if (id == IDC_TEXT_EDIT && code == EN_CHANGE)
        {
            Settings_UpdateCount(hwnd);
            return 0;
        }

        if (id == IDC_OPACITY_TRACK && code == 0)
        {
            const int pos = (int)SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_TRACK), TBM_GETPOS, 0, 0);
            SetEditInt(GetDlgItem(hwnd, IDC_OPACITY_EDIT), pos);
            return 0;
        }

        if (id == IDC_OPACITY_EDIT && code == EN_CHANGE)
        {
            int val = GetEditInt(GetDlgItem(hwnd, IDC_OPACITY_EDIT));
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_TRACK), TBM_SETPOS, TRUE, val);
            return 0;
        }

        if (id == IDC_SAVE && code == BN_CLICKED)
        {
            std::wstring error;
            if (!Settings_SaveFromControls(hwnd, error))
            {
                MessageBoxW(hwnd, error.c_str(), L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }

        if (id == IDC_PREVIEW && code == BN_CLICKED)
        {
            if (Overlay_IsVisible())
            {
                MessageBoxW(hwnd, L"屏保页正在显示中，无法预览。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            std::wstring error;
            AppConfig candidate{};
            if (!Settings_TryBuildCandidateFromControls(hwnd, candidate, error))
            {
                MessageBoxW(hwnd, error.c_str(), L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            Scheduler_Stop(g_hwndMain);
            Overlay_ShowWithConfig(g_hwndMain, candidate);
            return 0;
        }

        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void Settings_Show(HWND hwndOwner)
{
    if (!g_hwndSettings)
    {
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        g_hwndSettings = CreateWindowExW(
            WS_EX_APPWINDOW,
            L"SSR_SETTINGS",
            L"屏保提醒工具 - 设置",
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            440, 430,
            hwndOwner, nullptr, g_hInstance, nullptr
        );
    }

    if (!g_hwndSettings)
    {
        return;
    }

    Settings_LoadToControls(g_hwndSettings);
    ShowWindow(g_hwndSettings, SW_SHOW);
    SetForegroundWindow(g_hwndSettings);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        LoadConfig(g_config);
        Tray_Create(hwnd);
        Scheduler_Start(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_INTERVAL)
        {
            Scheduler_Stop(hwnd);
            Overlay_Show(hwnd);
            return 0;
        }
        if (wParam == TIMER_OVERLAY_ANIM)
        {
            Overlay_TickAnim(hwnd);
            return 0;
        }
        if (wParam == TIMER_OVERLAY_CLOCK)
        {
            if (Overlay_IsVisible())
            {
                Overlay_InvalidateAll();
            }
            return 0;
        }
        return 0;
    case WMAPP_TRAY:
    {
        if (lParam == WM_RBUTTONUP)
        {
            Tray_ShowMenu(hwnd);
            return 0;
        }
        if (lParam == WM_LBUTTONDBLCLK)
        {
            Settings_Show(hwnd);
            return 0;
        }
        return 0;
    }
    case WMAPP_ACTIVITY:
        if (g_overlayState.load() == OverlayState::WaitingInput)
        {
            Overlay_BeginFadeOut(hwnd);
        }
        return 0;
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        if (id == IDM_TRAY_OPEN_SETTINGS)
        {
            Settings_Show(hwnd);
            return 0;
        }
        if (id == IDM_TRAY_EXIT)
        {
            App_Exit(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        Tray_Destroy();
        Scheduler_Stop(hwnd);
        InputMonitor_Stop();
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static ATOM RegisterWindowClass(const wchar_t* name, WNDPROC proc, HICON icon, HBRUSH backgroundBrush)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInstance;
    wc.lpszClassName = name;
    wc.lpfnWndProc = proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hbrBackground = backgroundBrush;
    return RegisterClassExW(&wc);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_hInstance = hInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    g_settingsBgBrush = CreateSolidBrush(RGB(250, 250, 250));

    HICON icon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_FAVICON));
    RegisterWindowClass(L"SSR_MAIN", MainWndProc, icon, (HBRUSH)GetStockObject(WHITE_BRUSH));
    RegisterWindowClass(L"SSR_SETTINGS", SettingsWndProc, icon, g_settingsBgBrush ? g_settingsBgBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
    RegisterWindowClass(L"SSR_OVERLAY", OverlayWndProc, icon, (HBRUSH)GetStockObject(NULL_BRUSH));

    g_hwndMain = CreateWindowExW(
        0,
        L"SSR_MAIN",
        L"ScreenSaverReminderCPP",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hwndMain)
    {
        return 1;
    }

    ShowWindow(g_hwndMain, SW_HIDE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_settingsBgBrush)
    {
        DeleteObject(g_settingsBgBrush);
        g_settingsBgBrush = nullptr;
    }

    CoUninitialize();
    return 0;
}
