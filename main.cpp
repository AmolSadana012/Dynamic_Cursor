#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>

namespace
{
constexpr UINT WM_MOUSE_SAMPLE = WM_APP + 1;
constexpr UINT WM_SHUTDOWN_REQUEST = WM_APP + 2;
constexpr UINT TIMER_DECAY = 1;
constexpr UINT kArrowCursorRole = 32512; // OCR_NORMAL / IDC_ARROW
constexpr int kSizeLevels[] = {48, 64, 80, 96};
constexpr size_t kMaximumSamples = 96;
constexpr DWORD kSampleWindowMs = 900;
constexpr ULONGLONG kDetectionCooldownMs = 280;
constexpr ULONGLONG kDecayDelayMs = 300;
constexpr double kPi = 3.14159265358979323846;

struct MouseSample
{
    POINT point;
    DWORD time;
};

HHOOK g_mouseHook = nullptr;
HWND g_window = nullptr;
HCURSOR g_savedCursor = nullptr;
int g_sizeLevel = 0;
ULONGLONG g_lastMovement = 0;
ULONGLONG g_lastDetection = 0;
std::deque<MouseSample> g_samples;

void ReportWin32Error()
{
    // Keep failures observable during debugging without showing UI from a hook.
    OutputDebugString(L"Dynamic Cursor: Win32 operation failed.\n");
}

HCURSOR CreateScaledCursor(HCURSOR source, int size)
{
    if (!source || size <= 0)
        return nullptr;

    ICONINFO sourceInfo{};
    if (!GetIconInfo(source, &sourceInfo))
        return nullptr;

    HBITMAP dimensionsBitmap = sourceInfo.hbmColor != nullptr
        ? sourceInfo.hbmColor
        : sourceInfo.hbmMask;
    BITMAP dimensions{};
    const bool dimensionsValid =
        dimensionsBitmap != nullptr &&
        GetObject(dimensionsBitmap, sizeof(dimensions), &dimensions) ==
            sizeof(dimensions);
    const int sourceWidth = dimensionsValid
        ? std::max(1L, dimensions.bmWidth)
        : 1;
    // A monochrome cursor stores color and mask planes one above the other.
    const int sourceHeight = dimensionsValid
        ? std::max(1L, sourceInfo.hbmColor
            ? dimensions.bmHeight
            : dimensions.bmHeight / 2)
        : 1;

    HDC screen = GetDC(nullptr);
    HDC colorDc = screen ? CreateCompatibleDC(screen) : nullptr;
    HDC maskDc = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP colorBitmap = screen
        ? CreateCompatibleBitmap(screen, size, size)
        : nullptr;
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);

    if (!screen || !colorDc || !maskDc || !colorBitmap || !maskBitmap)
    {
        if (colorBitmap)
            DeleteObject(colorBitmap);
        if (maskBitmap)
            DeleteObject(maskBitmap);
        if (colorDc)
            DeleteDC(colorDc);
        if (maskDc)
            DeleteDC(maskDc);
        if (screen)
            ReleaseDC(nullptr, screen);
        DeleteObject(sourceInfo.hbmColor);
        DeleteObject(sourceInfo.hbmMask);
        ReportWin32Error();
        return nullptr;
    }

    HGDIOBJ oldColor = SelectObject(colorDc, colorBitmap);
    HGDIOBJ oldMask = SelectObject(maskDc, maskBitmap);
    PatBlt(colorDc, 0, 0, size, size, BLACKNESS);
    PatBlt(maskDc, 0, 0, size, size, WHITENESS);
    DrawIconEx(colorDc, 0, 0, source, size, size, 0, nullptr, DI_NORMAL);
    DrawIconEx(maskDc, 0, 0, source, size, size, 0, nullptr, DI_MASK);
    SelectObject(colorDc, oldColor);
    SelectObject(maskDc, oldMask);

    ICONINFO resultInfo{};
    resultInfo.fIcon = FALSE;
    resultInfo.xHotspot = static_cast<DWORD>(std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(sourceInfo.xHotspot) * size / sourceWidth)),
        0, size - 1));
    resultInfo.yHotspot = static_cast<DWORD>(std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(sourceInfo.yHotspot) * size / sourceHeight)),
        0, size - 1));
    resultInfo.hbmColor = colorBitmap;
    resultInfo.hbmMask = maskBitmap;

    HCURSOR result = CreateIconIndirect(&resultInfo);
    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);
    DeleteDC(colorDc);
    DeleteDC(maskDc);
    ReleaseDC(nullptr, screen);
    DeleteObject(sourceInfo.hbmColor);
    DeleteObject(sourceInfo.hbmMask);
    if (!result)
        ReportWin32Error();
    return result;
}

bool InstallCursor(HCURSOR cursor)
{
    if (!cursor || !SetSystemCursor(cursor, kArrowCursorRole))
    {
        if (cursor)
            DestroyCursor(cursor);
        ReportWin32Error();
        return false;
    }
    // SetSystemCursor owns and destroys a cursor after a successful call.
    return true;
}

bool InstallCursorSize(int size)
{
    HCURSOR replacement = CreateScaledCursor(g_savedCursor, size);
    return replacement != nullptr && InstallCursor(replacement);
}

bool RestoreOriginalCursor()
{
    if (!g_savedCursor)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        HCURSOR replacement = CopyCursor(g_savedCursor);
        if (replacement && InstallCursor(replacement))
        {
            g_sizeLevel = 0;
            return true;
        }
    }

    ReportWin32Error();
    return false;
}

void ClearQueuedSamples()
{
    MSG message{};
    while (PeekMessage(&message, g_window, WM_MOUSE_SAMPLE,
                       WM_MOUSE_SAMPLE, PM_REMOVE))
    {
        delete reinterpret_cast<MouseSample*>(message.lParam);
    }
    g_samples.clear();
}

void Shutdown()
{
    if (g_window)
        KillTimer(g_window, TIMER_DECAY);
    if (g_mouseHook)
    {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }

    ClearQueuedSamples();
    RestoreOriginalCursor();

    if (g_savedCursor)
    {
        DestroyCursor(g_savedCursor);
        g_savedCursor = nullptr;
    }
    g_sizeLevel = 0;
}

bool HasRapidReversal()
{
    if (g_samples.size() < 5)
        return false;

    int reversals = 0;
    int previousDirection = 0;
    double path = 0;
    for (size_t i = 1; i < g_samples.size(); ++i)
    {
        const LONG dx = g_samples[i].point.x - g_samples[i - 1].point.x;
        const LONG dy = g_samples[i].point.y - g_samples[i - 1].point.y;
        const double dominant = std::max(std::abs(dx), std::abs(dy));
        const double minor = std::min(std::abs(dx), std::abs(dy));
        if (dominant < 10 || dominant < minor * 1.25)
            continue;

        const int direction = std::abs(dx) >= std::abs(dy)
            ? (dx > 0 ? 1 : -1)
            : (dy > 0 ? 1 : -1);
        path += dominant;
        if (previousDirection != 0 && direction != previousDirection)
            ++reversals;
        previousDirection = direction;
    }
    return reversals >= 3 && path >= 260;
}

bool HasCircularMotion()
{
    if (g_samples.size() < 8)
        return false;

    double path = 0;
    double signedTurning = 0;
    double absoluteTurning = 0;
    size_t turns = 0;
    int turnSign = 0;
    int consistentTurns = 0;
    POINT first = g_samples.front().point;
    POINT last = g_samples.back().point;
    LONG minX = first.x;
    LONG maxX = first.x;
    LONG minY = first.y;
    LONG maxY = first.y;

    for (size_t i = 1; i < g_samples.size(); ++i)
    {
        minX = std::min(minX, g_samples[i].point.x);
        maxX = std::max(maxX, g_samples[i].point.x);
        minY = std::min(minY, g_samples[i].point.y);
        maxY = std::max(maxY, g_samples[i].point.y);
    }

    for (size_t i = 2; i < g_samples.size(); ++i)
    {
        const double ax = g_samples[i - 1].point.x - g_samples[i - 2].point.x;
        const double ay = g_samples[i - 1].point.y - g_samples[i - 2].point.y;
        const double bx = g_samples[i].point.x - g_samples[i - 1].point.x;
        const double by = g_samples[i].point.y - g_samples[i - 1].point.y;
        const double firstLength = std::hypot(ax, ay);
        const double secondLength = std::hypot(bx, by);
        if (firstLength < 4 || secondLength < 4)
            continue;

        path += secondLength;
        const double turn = std::atan2(ax * by - ay * bx, ax * bx + ay * by);
        if (std::abs(turn) < 0.12)
            continue;
        signedTurning += turn;
        absoluteTurning += std::abs(turn);
        ++turns;
        const int currentSign = turn > 0 ? 1 : -1;
        if (turnSign == 0 || currentSign == turnSign)
            ++consistentTurns;
        turnSign = currentSign;
    }

    const double displacement = std::hypot(
        static_cast<double>(last.x - first.x),
        static_cast<double>(last.y - first.y));
    const double consistency = absoluteTurning > 0
        ? std::abs(signedTurning) / absoluteTurning
        : 0;
    return path >= 260 &&
        turns >= 5 &&
        (maxX - minX) >= 35 &&
        (maxY - minY) >= 35 &&
        std::abs(signedTurning) >= kPi * 1.5 &&
        consistency >= 0.62 &&
        consistentTurns >= turns * 0.70 &&
        displacement < path * 0.78;
}

void HandleMouseSample(const MouseSample& sample)
{
    g_lastMovement = GetTickCount64();
    g_samples.push_back(sample);
    while (!g_samples.empty() &&
           (sample.time - g_samples.front().time > kSampleWindowMs ||
            g_samples.size() > kMaximumSamples))
    {
        g_samples.pop_front();
    }

    const ULONGLONG now = GetTickCount64();
    if ((HasRapidReversal() || HasCircularMotion()) &&
        now - g_lastDetection >= kDetectionCooldownMs &&
        g_sizeLevel < static_cast<int>(std::size(kSizeLevels)))
    {
        if (InstallCursorSize(kSizeLevels[g_sizeLevel]))
        {
            ++g_sizeLevel;
            g_lastDetection = now;
        }
    }
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT ||
        controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT ||
        controlType == CTRL_LOGOFF_EVENT ||
        controlType == CTRL_SHUTDOWN_EVENT)
    {
        if (g_window)
            PostMessage(g_window, WM_SHUTDOWN_REQUEST, 0, 0);
        return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == WM_MOUSEMOVE && g_window)
    {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        auto* sample = new MouseSample{data->pt, data->time};
        if (!PostMessage(g_window, WM_MOUSE_SAMPLE, 0,
                         reinterpret_cast<LPARAM>(sample)))
        {
            delete sample;
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSE_SAMPLE:
    {
        auto* sample = reinterpret_cast<MouseSample*>(lParam);
        if (sample)
        {
            HandleMouseSample(*sample);
            delete sample;
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == TIMER_DECAY &&
            g_sizeLevel > 0 &&
            GetTickCount64() - g_lastMovement >= kDecayDelayMs)
        {
            if (g_sizeLevel == 1)
                RestoreOriginalCursor();
            else if (InstallCursorSize(kSizeLevels[g_sizeLevel - 2]))
                --g_sizeLevel;
            g_samples.clear();
        }
        return 0;
    case WM_SHUTDOWN_REQUEST:
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_QUERYENDSESSION:
        Shutdown();
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
}
} // namespace

int wmain()
{
    const HINSTANCE instance = GetModuleHandle(nullptr);
    WNDCLASS windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = L"DynamicCursorWindow";
    if (!RegisterClass(&windowClass))
        return 1;

    g_window = CreateWindowEx(
        0, windowClass.lpszClassName, L"Dynamic Cursor",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_window)
        return 1;

    HCURSOR currentCursor = LoadCursor(nullptr, IDC_ARROW);
    g_savedCursor = currentCursor ? CopyCursor(currentCursor) : nullptr;
    if (!g_savedCursor)
    {
        DestroyWindow(g_window);
        g_window = nullptr;
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, nullptr, 0);
    if (!g_mouseHook || !SetTimer(g_window, TIMER_DECAY, 60, nullptr))
    {
        Shutdown();
        DestroyWindow(g_window);
        g_window = nullptr;
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        return 1;
    }

    MSG message{};
    int result = 0;
    while ((result = GetMessage(&message, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    if (result < 0)
        ReportWin32Error();
    Shutdown();
    g_window = nullptr;
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    return result < 0 ? 1 : 0;
}
