#include <windows.h>

#include "../../bh_agent_log.h"

#include <cstdio>
#include <string>

static const wchar_t* g_window_class_name = L"TargetAppWindowClass";
static std::wstring g_window_config_path;
static int g_counter = 0;
static ULONGLONG g_start_tick_ms = 0;

static void log_timing(const char* tag)
{
    if (!tag)
    {
        return;
    }

    const ULONGLONG now_ms = GetTickCount64();
    if (g_start_tick_ms == 0)
    {
        g_start_tick_ms = now_ms;
    }

    bh_agent_logf(
        "target.app",
        "timing +%llums pid=%lu tid=%lu %s",
        (unsigned long long)(now_ms - g_start_tick_ms),
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetCurrentThreadId(),
        tag);
}

static bool initialize_window_config_path()
{
    if (!g_window_config_path.empty())
    {
        return true;
    }

    wchar_t local_app_data[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return false;
    }

    std::wstring directory = std::wstring(local_app_data) + L"\\bh_agent_debugger";
    CreateDirectoryW(directory.c_str(), nullptr);
    g_window_config_path = directory + L"\\targetapp_window.cfg";
    return true;
}

static void save_window_position(HWND window)
{
    if (!window || !initialize_window_config_path())
    {
        return;
    }

    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement))
    {
        return;
    }

    RECT rect = placement.rcNormalPosition;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 100 || height <= 100)
    {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_window_config_path.c_str(), L"w") != 0 || !file)
    {
        return;
    }

    fwprintf(file, L"%d %d %d %d\n", rect.left, rect.top, width, height);
    fclose(file);
}

static void load_window_position(int& left, int& top, int& width, int& height)
{
    left = CW_USEDEFAULT;
    top = CW_USEDEFAULT;
    width = 920;
    height = 420;

    if (!initialize_window_config_path())
    {
        return;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_window_config_path.c_str(), L"r") != 0 || !file)
    {
        return;
    }

    int scanned = fwscanf_s(file, L"%d %d %d %d", &left, &top, &width, &height);
    fclose(file);
    if (scanned != 4 || width <= 100 || height <= 100)
    {
        left = CW_USEDEFAULT;
        top = CW_USEDEFAULT;
        width = 920;
        height = 420;
    }
}

extern "C" __declspec(noinline) int target_work(int value){
    int result = value * 3 + 7;
    return result;
}

extern "C" __declspec(dllexport) __declspec(noinline) void bh_agent_dbg_on_breakpoint_hit()
{
    volatile int marker = 1;
    (void)marker;
}

static void target_loop_tick(HWND window)
{
    int value = target_work(g_counter);
    char text[128] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "TargetApp running: counter=%d value=%d", g_counter, value);
    SetWindowTextA(window, text);
    ++g_counter;
}

static LRESULT CALLBACK target_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CREATE:
        log_timing("WM_CREATE");
        SetTimer(window, 1, 1000, nullptr);
        return 0;
    case WM_TIMER:
        if (w_param == 1)
        {
            target_loop_tick(window);
        }
        return 0;
    case WM_CLOSE:
        save_window_position(window);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(window, message, w_param, l_param);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd)
{
    g_start_tick_ms = GetTickCount64();
    log_timing("wWinMain entry");

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = target_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window_class.lpszClassName = g_window_class_name;

    if (!RegisterClassExW(&window_class))
    {
        return 1;
    }

    int left = CW_USEDEFAULT;
    int top = CW_USEDEFAULT;
    int width = 920;
    int height = 420;
    load_window_position(left, top, width, height);

    HWND window = CreateWindowExW(
        0,
        g_window_class_name,
        L"TargetApp",
        WS_OVERLAPPEDWINDOW,
        left,
        top,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!window)
    {
        return 1;
    }

    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    placement.flags = 0;
    placement.showCmd = SW_HIDE;
    placement.ptMinPosition = { -1, -1 };
    placement.ptMaxPosition = { -1, -1 };
    placement.rcNormalPosition = { left, top, left + width, top + height };
    SetWindowPlacement(window, &placement);

    SetActiveWindow(window);
    InvalidateRect(window, nullptr, TRUE);

    ShowWindow(window, show_cmd);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessage(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    return (int)message.wParam;
}
