#include <windows.h>
#include <commctrl.h>

#include "logger.h"
#include "window/main_window.h"
#include "string_utils.h"
#include "indexer.h"
#include "resource.h"
#include "config/user_config.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

// 启用 ComCtl32 v6 视觉样式（PBS_MARQUEE 进度条需要）
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


static constexpr wchar_t kAppClassName[] = L"EveryZipMainWindow";
static constexpr wchar_t kAppTitle[] = L"EveryZip";
static constexpr int kDefaultWindowWidth = 1100;
static constexpr int kDefaultWindowHeight = 620;

 // 初始化程序依赖的公共控件类，确保 ListView、状态栏和进度条可用。
 // 参数：无。
 // 返回值：无。
static void EnsureCommonControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);
}

static bool IsSavedWindowRectUsable(const UserConfig::WindowPlacementConfig& placement) {
    RECT rc{
        placement.left,
        placement.top,
        placement.right,
        placement.bottom
    };

    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return false;
    }

    HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return false;
    }

    RECT intersection{};
    return IntersectRect(&intersection, &rc, &mi.rcWork) != FALSE;
}

static UINT GetPrimaryMonitorDpi() {
    POINT origin{ 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

    using FnGetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static HMODULE shcore = LoadLibraryW(L"shcore.dll");
    static auto fn = shcore
        ? (FnGetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor")
        : nullptr;
    if (fn && monitor) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        static constexpr int kMdtEffectiveDpi = 0;
        if (SUCCEEDED(fn(monitor, kMdtEffectiveDpi, &dpiX, &dpiY)) && dpiX > 0) {
            return dpiX;
        }
    }

    HDC hdc = GetDC(nullptr);
    UINT dpi = 0;
    if (hdc) {
        dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
    }
    return dpi ? dpi : 96;
}

static int ScaleDpi(int value, UINT dpi) {
    return MulDiv(value, (int)dpi, 96);
}

static void CenterWindowOnPrimaryWorkArea(int width, int height, int* outX, int* outY) {
    if (!outX || !outY) return;

    POINT origin{ 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) {
        *outX = CW_USEDEFAULT;
        *outY = CW_USEDEFAULT;
        return;
    }

    const int workWidth = mi.rcWork.right - mi.rcWork.left;
    const int workHeight = mi.rcWork.bottom - mi.rcWork.top;
    *outX = mi.rcWork.left + max(0, (workWidth - width) / 2);
    *outY = mi.rcWork.top + max(0, (workHeight - height) / 2);
}

// ══════════════════════════════════════════════════════════════
//  程序入口
// ══════════════════════════════════════════════════════════════
 // 初始化日志、配置、窗口和消息循环，是应用程序的主入口。
 // 参数：hInstance - 当前实例句柄；第二个 HINSTANCE 未使用；PWSTR 未使用；nCmdShow - 初始窗口显示方式。
 // 返回值：进程退出码。
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const uint64_t appStartTickMs = GetTickCount64();

    // 单实例检测：创建命名 Mutex，已存在说明有实例在运行
    static constexpr wchar_t kMutexName[] = L"EveryZip_SingleInstance_Mutex";
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例：找到其主窗口并激活显示
        HWND hExisting = FindWindowW(kAppClassName, nullptr);
        if (hExisting) {
            if (IsIconic(hExisting)) {
                ShowWindow(hExisting, SW_RESTORE);
            } else {
                ShowWindow(hExisting, SW_SHOW);
            }
            SetForegroundWindow(hExisting);
        }
        CloseHandle(hMutex);
        return 0;
    }

    Logger::Init();
    LOG_INFO("wWinMain start");

    std::wstring exeDir = GetExeDir();
    std::wstring dbPath = exeDir + L"\\everyzip.ezdb";
    std::wstring configPath = exeDir + L"\\everyzip.cfg";

    const bool p1 = Indexer::EnablePrivilege(SE_MANAGE_VOLUME_NAME);
    const bool p2 = Indexer::EnablePrivilege(SE_BACKUP_NAME);
    LOG_INFO("EnablePrivilege SeManageVolume=%d SeBackup=%d", (int)p1, (int)p2);

    EnsureCommonControls();

    // 创建主窗口状态（通过 CREATESTRUCT::lpCreateParams 传递给 WndProc）
    MainWindowState state;
    state.hInstance = hInstance;
    state.dbPath = dbPath;
    {
        std::wstring cfgErr;
        if (!state.userConfig.Load(configPath, &cfgErr)) {
            LOG_WARN(L"UserConfig::Load failed: %s", cfgErr.c_str());
        }
    }
    state.indexer.SetDbPath(dbPath);
    state.indexer.SetArchiveFormatRules(state.userConfig.GetArchiveFormatRules());
    state.indexer.SetScanDriveLetters(state.userConfig.GetScanDriveLetters());
    state.indexer.SetParseThreadCount(state.userConfig.GetParseThreadCount());
    state.indexer.SetAppStartTickMs(appStartTickMs);
    state.showArchiveFullPath = state.userConfig.GetShowArchiveFullPath();
    state.rowCache.SetDbPath(dbPath);
    state.rowCache.SetIconCache(&state.iconCache);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kAppClassName;

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("RegisterClassExW failed (err=%lu)", GetLastError());
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    const UINT initialDpi = GetPrimaryMonitorDpi();
    int windowWidth = ScaleDpi(kDefaultWindowWidth, initialDpi);
    int windowHeight = ScaleDpi(kDefaultWindowHeight, initialDpi);
    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;
    int initialShowCmd = nCmdShow;
    CenterWindowOnPrimaryWorkArea(windowWidth, windowHeight, &windowX, &windowY);

    if (state.userConfig.GetRememberUiState()) {
        const auto& placement = state.userConfig.GetWindowPlacement();
        if (IsSavedWindowRectUsable(placement)) {
            windowX = placement.left;
            windowY = placement.top;
            windowWidth = placement.right - placement.left;
            windowHeight = placement.bottom - placement.top;
            if (placement.maximized) {
                initialShowCmd = SW_SHOWMAXIMIZED;
            }
        }
    }

    HWND hWnd = CreateWindowExW(
        0,
        kAppClassName,
        kAppTitle,
        WS_OVERLAPPEDWINDOW,
        windowX, windowY,
        windowWidth, windowHeight,
        nullptr,
        nullptr,
        hInstance,
        &state);  // 传递 state 指针

    if (!hWnd) {
        LOG_ERROR("CreateWindowExW main window failed (err=%lu)", GetLastError());
        MessageBoxW(nullptr, L"CreateWindowExW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hWnd, initialShowCmd);
    UpdateWindow(hWnd);

    // 主消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOG_INFO("Message loop exit code=%lld", (long long)msg.wParam);
    Logger::Shutdown();
    return (int)msg.wParam;
}
