// ── ui.cpp ──────────────────────────────────────────
//   Win32 主窗口创建、控件布局、消息处理 (WndProc)
//   所有耗时操作均在工作线程中异步执行，通过自定义 WM_USER
//   消息与 UI 线程通信，确保窗口不卡死。
// ─────────────────────────────────────────────────────

#include "ui.h"
#include "network.h"
#include "utils.h"

#include <commctrl.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <cstdio>
#include <vector>
#include <memory>

// ============================================================
// 全局控件和状态
// ============================================================
static HWND      g_hWnd      = nullptr;    // 主窗口
static HWND      g_hListView = nullptr;    // 网卡列表
static HWND      g_hMtuEdit  = nullptr;    // MTU 输入框
static HWND      g_hPingAddr = nullptr;    // Ping 目标地址输入框
static HWND      g_hLogEdit  = nullptr;    // 日志面板
static HWND      g_hSplitter = nullptr;    // 分割条
static HINSTANCE g_hInst     = nullptr;    // 实例句柄
static HFONT     g_hFont     = nullptr;    // 全局字体

// 日志面板高度（可通过拖拽分割条调整）
static int g_logHeight = 120;

// 分割条拖拽状态
static bool  g_splitterDragging = false;
static POINT g_splitterDragStart = {};

// 当前枚举到的网卡列表（仅在 UI 线程访问，或加锁保护）
static std::vector<AdapterInfo> g_adapters;
static SRWLOCK                  g_adaptersLock = SRWLOCK_INIT;

// 操作忙状态标志（防止重复点击）
static LONG g_busy = 0;

// 窗口类名
static const wchar_t* g_szClassName = L"MTUTool_MainWindow";

// ============================================================
// 前向声明
// ============================================================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void    OnSize(HWND hWnd, int cx, int cy);
static void    OnCreate(HWND hWnd);
static void    RefreshAdapterList();
static void    ApplyMTU(uint32_t mtu);
static void    StartMtuValidation(uint32_t mtu);
static void    RunGatewayTest();
static void    RunRestoreDefault();
static void    RunPingTest();
static void    RunMtuTest();
static void    AppendLog(const wchar_t* text);
static void    SetButtonsEnabled(BOOL enabled);
static bool    TryEnterBusy();

// ============================================================
// 外部可见 API
// ============================================================

bool CreateMainWindow(HINSTANCE hInstance) {
    g_hInst = hInstance;

    // 初始化 Common Controls（ListView 等需要）
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // ── 注册窗口类 ──
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(101));  // IDI_MAIN
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = g_szClassName;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // ── 创建窗口 ──
    g_hWnd = CreateWindowExW(
        0,
        g_szClassName,
        L"本地网卡 MTU 修改工具 v2.0",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        780, 560,
        nullptr, nullptr,
        hInstance, nullptr
    );

    if (!g_hWnd) {
        return false;
    }

    // 设置最小窗口大小
    // (通过处理 WM_GETMINMAXINFO 实现，见 WndProc)

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    return true;
}

int RunMessageLoop() {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

HWND GetMainWindow()     { return g_hWnd; }
HWND GetAdapterListView() { return g_hListView; }

// ============================================================
// 工具函数
// ============================================================

// 尝试进入忙碌状态，返回 true 表示成功获取锁
static bool TryEnterBusy() {
    return InterlockedCompareExchange(&g_busy, 1, 0) == 0;
}

// 退出忙碌状态
static void LeaveBusy() {
    InterlockedExchange(&g_busy, 0);
}

// 统一设置按钮启用/禁用状态
static void SetButtonsEnabled(BOOL enabled) {
    EnableWindow(GetDlgItem(g_hWnd, IDC_APPLY_BTN),    enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PRESET_1500),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PRESET_1492),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PRESET_1472),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PRESET_9000),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_GET_GATEWAY),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_RESTORE_BTN),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_REFRESH_BTN),  enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PING_BTN),     enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_PING_ADDR),    enabled);
    EnableWindow(GetDlgItem(g_hWnd, IDC_MTU_TEST_BTN), enabled);
}

// 获取当前在 ListView 中选中的项目索引（-1 表示未选中）
static int GetSelectedAdapterIndex() {
    return ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
}

// 从 ListView 中获取指定索引的网卡名称（第 0 列）
static std::wstring GetAdapterNameFromList(int index) {
    wchar_t buf[256] = {};
    LVITEMW lvi = {};
    lvi.mask       = LVIF_TEXT;
    lvi.iSubItem   = 0;
    lvi.pszText    = buf;
    lvi.cchTextMax = static_cast<int>(_countof(buf));
    // 使用 SendMessage 而非宏，避免参数不匹配
    SendMessageW(g_hListView, LVM_GETITEMTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&lvi));
    return buf;
}

// 将日志文本追加到日志面板并自动滚动到底部
static void AppendLog(const wchar_t* text) {
    if (!g_hLogEdit) return;

    // 获取当前文本长度
    int len = GetWindowTextLengthW(g_hLogEdit);

    // 将光标移到末尾并追加文本
    SendMessageW(g_hLogEdit, EM_SETSEL, len, len);

    // 添加时间戳前缀
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%02d:%02d:%02d  %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, text);

    SendMessageW(g_hLogEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line));

    // 自动滚动到底部
    SendMessageW(g_hLogEdit, EM_SCROLLCARET, 0, 0);
}

// 日志回调（供 utils.cpp 中的通用日志函数使用）
static void LogCallbackImpl(const wchar_t* msg) {
    AppendLog(msg);
}

// ============================================================
// 控件创建（由 WM_CREATE 调用）
// ============================================================

static void OnCreate(HWND hWnd) {
    g_hWnd = hWnd;
    SetLogCallback(LogCallbackImpl);

    // ── 创建字体（Segoe UI 9pt 或默认 GUI 字体）──
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    // ── ListView 控件 ──
    g_hListView = CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,   // 位置由 WM_SIZE 设置
        hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LISTVIEW)),
        g_hInst,
        nullptr
    );

    // 设置 ListView 扩展样式
    ListView_SetExtendedListViewStyle(g_hListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // 添加列
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    col.pszText = const_cast<LPWSTR>(L"网卡名称");
    col.cx      = 260;
    col.fmt     = LVCFMT_LEFT;
    ListView_InsertColumn(g_hListView, 0, &col);

    col.pszText = const_cast<LPWSTR>(L"当前MTU");
    col.cx      = 80;
    col.fmt     = LVCFMT_CENTER;
    ListView_InsertColumn(g_hListView, 1, &col);

    col.pszText = const_cast<LPWSTR>(L"MAC地址");
    col.cx      = 150;
    col.fmt     = LVCFMT_CENTER;
    ListView_InsertColumn(g_hListView, 2, &col);

    col.pszText = const_cast<LPWSTR>(L"状态");
    col.cx      = 70;
    col.fmt     = LVCFMT_CENTER;
    ListView_InsertColumn(g_hListView, 3, &col);

    // ── "修改MTU:" 标签 ──
    CreateWindowExW(0, WC_STATICW, L"修改MTU:",
                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MTU_LABEL)),
                    g_hInst, nullptr);

    // ── MTU 输入框（仅允许数字）──
    g_hMtuEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"1500",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
        0, 0, 0, 0,
        hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MTU_EDIT)),
        g_hInst, nullptr
    );

    // 限制输入长度
    SendMessageW(g_hMtuEdit, EM_SETLIMITTEXT, 4, 0);

    // ── "应用" 按钮 ──
    CreateWindowExW(0, WC_BUTTONW, L"应用",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_APPLY_BTN)),
                    g_hInst, nullptr);

    // ── "预设:" 标签 ──
    CreateWindowExW(0, WC_STATICW, L"预设:",
                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET_LABEL)),
                    g_hInst, nullptr);

    // ── 预设按钮 ──
    CreateWindowExW(0, WC_BUTTONW, L"1500 标准以太网",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET_1500)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"1492 PPPoE",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET_1492)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"1472 VPN",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET_1472)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"9000 巨型帧",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET_9000)),
                    g_hInst, nullptr);

    // ── 操作按钮行 ──
    CreateWindowExW(0, WC_BUTTONW, L"获取网关",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GET_GATEWAY)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"恢复默认",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESTORE_BTN)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"刷新列表",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH_BTN)),
                    g_hInst, nullptr);

    // ── "Ping目标:" 标签 ──
    CreateWindowExW(0, WC_STATICW, L"Ping目标:",
                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PING_LABEL)),
                    g_hInst, nullptr);

    // ── Ping 目标地址输入框 ──
    g_hPingAddr = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"223.5.5.5",
        WS_CHILD | WS_VISIBLE | ES_LEFT,
        0, 0, 0, 0,
        hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PING_ADDR)),
        g_hInst, nullptr
    );

    CreateWindowExW(0, WC_BUTTONW, L"Ping 测试",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PING_BTN)),
                    g_hInst, nullptr);

    CreateWindowExW(0, WC_BUTTONW, L"MTU 测试",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 0, 0,
                    hWnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MTU_TEST_BTN)),
                    g_hInst, nullptr);

    // ── 日志面板 ──
    g_hLogEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
        WS_VSCROLL,
        0, 0, 0, 0,
        hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG_EDIT)),
        g_hInst, nullptr
    );

    // ── 分割条（拖拽调整列表和日志高度）──
    g_hSplitter = CreateWindowExW(
        0, WC_STATICW, L"",
        WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_CENTERIMAGE,
        0, 0, 0, 0,
        hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPLITTER)),
        g_hInst, nullptr
    );

    // 为所有子控件设置字体
    EnumChildWindows(hWnd, [](HWND hChild, LPARAM lParam) -> BOOL {
        SendMessageW(hChild, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
        return TRUE;
    }, 0);

    WriteLog(L"MTU Tool v2.0 已启动");

    // 启动后台线程刷新网卡列表
    RefreshAdapterList();
}

// ============================================================
// 窗口布局（WM_SIZE）
// ============================================================

static void OnSize(HWND hWnd, int cx, int cy) {
    if (!g_hListView || cx <= 0 || cy <= 0) return;

    const int MARGIN     = 10;
    const int ROW_H      = 26;
    const int ROW_GAP    = 6;
    const int SPLITTER_H = 5;
    const int LABEL_W    = 62;
    const int EDIT_W     = 70;
    const int APPLY_W    = 55;
    const int PRESET_LBL_W = 38;
    const int PRESET_SM  = 75;   // 短预设按钮 (1472 VPN)
    const int PRESET_MD  = 95;   // 中预设按钮 (1492 PPPoE, 9000 巨型帧)
    const int PRESET_LG  = 120;  // 长预设按钮 (1500 标准以太网)
    const int BTN_GAP    = 5;
    const int TOOL_W     = 85;   // 工具按钮宽度
    const int GROUP_GAP  = 16;   // 组间距

    // 限制日志高度范围
    int minLogH = 40;
    int maxLogH = cy - 260;  // 至少保留 260px 给列表和按钮区
    if (g_logHeight < minLogH) g_logHeight = minLogH;
    if (g_logHeight > maxLogH) g_logHeight = maxLogH;

    // ── Y 坐标：从底部往上算 ──
    // 分割条在日志面板上方
    int splitterY = cy - g_logHeight - SPLITTER_H;
    int logY      = splitterY + SPLITTER_H;
    int row2Y     = splitterY - MARGIN - ROW_H;
    int row1Y     = row2Y - ROW_GAP - ROW_H;

    // ── ListView ──
    MoveWindow(g_hListView, MARGIN, MARGIN,
               cx - MARGIN * 2, row1Y - MARGIN * 2, TRUE);

    // ═══════════════════════════════════════════════════
    // Row 1: [修改MTU:] [input] [应用]  |  [预设:] [1500] [1492] [1472] [9000]
    // ═══════════════════════════════════════════════════
    int x = MARGIN;

    // "修改MTU:" 标签
    MoveWindow(GetDlgItem(hWnd, IDC_MTU_LABEL), x, row1Y + 3, LABEL_W, ROW_H, TRUE);
    x += LABEL_W + 3;

    // MTU 输入框
    MoveWindow(g_hMtuEdit, x, row1Y + 1, EDIT_W, ROW_H - 2, TRUE);
    x += EDIT_W + BTN_GAP;

    // "应用" 按钮
    MoveWindow(GetDlgItem(hWnd, IDC_APPLY_BTN), x, row1Y, APPLY_W, ROW_H, TRUE);
    x += APPLY_W + GROUP_GAP;

    // "预设:" 标签
    MoveWindow(GetDlgItem(hWnd, IDC_PRESET_LABEL), x, row1Y + 3, PRESET_LBL_W, ROW_H, TRUE);
    x += PRESET_LBL_W + 3;

    // 预设按钮
    MoveWindow(GetDlgItem(hWnd, IDC_PRESET_1500), x, row1Y, PRESET_LG, ROW_H, TRUE);
    x += PRESET_LG + BTN_GAP;
    MoveWindow(GetDlgItem(hWnd, IDC_PRESET_1492), x, row1Y, PRESET_MD, ROW_H, TRUE);
    x += PRESET_MD + BTN_GAP;
    MoveWindow(GetDlgItem(hWnd, IDC_PRESET_1472), x, row1Y, PRESET_SM, ROW_H, TRUE);
    x += PRESET_SM + BTN_GAP;
    MoveWindow(GetDlgItem(hWnd, IDC_PRESET_9000), x, row1Y, PRESET_MD, ROW_H, TRUE);

    // ═══════════════════════════════════════════════════
    // Row 2: [Ping目标:] [input] [Ping测试] [MTU测试]  |  [获取网关] [恢复默认] [刷新列表]
    // ═══════════════════════════════════════════════════
    x = MARGIN;

    // "Ping目标:" 标签
    MoveWindow(GetDlgItem(hWnd, IDC_PING_LABEL), x, row2Y + 3, LABEL_W, ROW_H, TRUE);
    x += LABEL_W + 3;

    // Ping 地址输入框
    MoveWindow(g_hPingAddr, x, row2Y + 1, 130, ROW_H - 2, TRUE);
    x += 130 + BTN_GAP;

    // "Ping 测试" 按钮
    MoveWindow(GetDlgItem(hWnd, IDC_PING_BTN), x, row2Y, 75, ROW_H, TRUE);
    x += 75 + BTN_GAP;

    // "MTU 测试" 按钮
    MoveWindow(GetDlgItem(hWnd, IDC_MTU_TEST_BTN), x, row2Y, 75, ROW_H, TRUE);
    x += 75 + GROUP_GAP;

    // 工具按钮组
    MoveWindow(GetDlgItem(hWnd, IDC_GET_GATEWAY), x, row2Y, TOOL_W, ROW_H, TRUE);
    x += TOOL_W + BTN_GAP;
    MoveWindow(GetDlgItem(hWnd, IDC_RESTORE_BTN), x, row2Y, TOOL_W, ROW_H, TRUE);
    x += TOOL_W + BTN_GAP;
    MoveWindow(GetDlgItem(hWnd, IDC_REFRESH_BTN), x, row2Y, TOOL_W, ROW_H, TRUE);

    // ── 分割条 ──
    MoveWindow(g_hSplitter, MARGIN, splitterY, cx - MARGIN * 2, SPLITTER_H, TRUE);

    // ── 日志面板 ──
    MoveWindow(g_hLogEdit, MARGIN, logY, cx - MARGIN * 2, g_logHeight, TRUE);
}

// ============================================================
// 网卡列表刷新（后台线程）
// ============================================================

// 后台线程入口：枚举网卡，完成后通过消息通知 UI 线程
static DWORD WINAPI EnumThreadProc(LPVOID) {
    auto* adapters = new std::vector<AdapterInfo>();

    if (EnumerateAdapters(*adapters)) {
        // 成功：通过 PostMessage 将结果发送回 UI 线程
        PostMessageW(g_hWnd, WM_USER_ENUM_DONE, TRUE, reinterpret_cast<LPARAM>(adapters));
    } else {
        delete adapters;
        PostMessageW(g_hWnd, WM_USER_ENUM_DONE, FALSE, 0);
    }

    return 0;
}

static void RefreshAdapterList() {
    if (!TryEnterBusy()) {
        return;   // 已有操作在进行中
    }

    SetButtonsEnabled(FALSE);
    WriteLog(L"[信息] 正在枚举网卡...");

    HANDLE hThread = CreateThread(nullptr, 0, EnumThreadProc, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);   // 不需要等待，通过消息通知
    } else {
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        WriteLog(L"[错误] 创建枚举线程失败");
    }
}

// 处理枚举完成消息（UI 线程）
static void OnEnumDone(BOOL success, std::vector<AdapterInfo>* adapters) {
    if (!success) {
        WriteLog(L"[错误] 网卡枚举失败");
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        return;
    }

    // ── 更新全局网卡列表 ──
    {
        AcquireSRWLockExclusive(&g_adaptersLock);
        g_adapters = std::move(*adapters);
        ReleaseSRWLockExclusive(&g_adaptersLock);
    }
    delete adapters;

    // ── 更新 ListView ──
    ListView_DeleteAllItems(g_hListView);

    AcquireSRWLockShared(&g_adaptersLock);
    for (size_t i = 0; i < g_adapters.size(); i++) {
        const auto& ad = g_adapters[i];

        // 插入行
        LVITEMW lvi = {};
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = static_cast<int>(i);
        lvi.pszText  = const_cast<LPWSTR>(ad.name.c_str());
        lvi.cchTextMax = 0;

        int idx = ListView_InsertItem(g_hListView, &lvi);
        if (idx == -1) continue;

        // 设置子项
        // MTU
        wchar_t mtuStr[16];
        if (ad.mtu > 0) {
            _snwprintf_s(mtuStr, _countof(mtuStr), _TRUNCATE, L"%lu", ad.mtu);
        } else {
            wcscpy_s(mtuStr, L"N/A");
        }
        ListView_SetItemText(g_hListView, idx, 1, mtuStr);

        // MAC
        ListView_SetItemText(g_hListView, idx, 2,
                             const_cast<LPWSTR>(ad.mac.c_str()));

        // 状态
        ListView_SetItemText(g_hListView, idx, 3,
                             const_cast<LPWSTR>(ad.connected ? L"已连接" : L"未连接"));
    }
    ReleaseSRWLockShared(&g_adaptersLock);

    WriteLog(L"[信息] 列表刷新完成，共 %zu 个网卡", g_adapters.size());

    LeaveBusy();
    SetButtonsEnabled(TRUE);
}

// ============================================================
// MTU 修改（后台线程）
// ============================================================

// 后台线程：执行 MTU 修改
struct MtuTaskParams {
    std::wstring adapterName;
    NET_IFINDEX  ifIndex;
    uint32_t     newMTU;
};

// 后台线程：执行 MTU 分片验证
struct MtuValidateParams {
    uint32_t mtu;
    wchar_t  target[256];
};

static DWORD WINAPI MtuThreadProc(LPVOID lpParam) {
    auto* params = static_cast<MtuTaskParams*>(lpParam);
    bool success = false;
    wchar_t resultMsg[512] = {};

    // ── 1. 尝试 API 方式 ──
    success = SetMTUviaAPI(params->ifIndex, params->newMTU);

    if (success) {
        _snwprintf_s(resultMsg, _countof(resultMsg), _TRUNCATE,
                     L"[成功] 已将 \"%s\" 的 MTU 修改为 %lu",
                     params->adapterName.c_str(), params->newMTU);
    } else {
        // ── 2. 回退到 netsh ──
        WriteLog(L"[信息] API 方式失败，尝试 netsh 回退方案...");
        success = SetMTUviaNetsh(params->adapterName, params->newMTU);

        if (success) {
            _snwprintf_s(resultMsg, _countof(resultMsg), _TRUNCATE,
                         L"[成功] 已通过 netsh 将 \"%s\" 的 MTU 修改为 %lu",
                         params->adapterName.c_str(), params->newMTU);
        } else {
            _snwprintf_s(resultMsg, _countof(resultMsg), _TRUNCATE,
                         L"[失败] 无法修改 \"%s\" 的 MTU（API 和 netsh 均失败）",
                         params->adapterName.c_str());
        }
    }

    // ── 3. 备份到注册表 ──
    if (success) {
        RegWriteMTUBackup(params->adapterName, params->newMTU);
    }

    // 分配消息字符串（UI 线程负责释放）
    auto* msgCopy = new wchar_t[lstrlenW(resultMsg) + 1];
    lstrcpyW(msgCopy, resultMsg);

    PostMessageW(g_hWnd, WM_USER_MTU_DONE, success ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(msgCopy));

    delete params;
    return 0;
}

// ============================================================
// MTU 分片验证（后台线程，在修改前执行）
// ============================================================

static DWORD WINAPI ValidateThreadProc(LPVOID lpParam) {
    auto* params = static_cast<MtuValidateParams*>(lpParam);

    MtuValidationResult result = ValidateMTU(params->target, params->mtu, 4000);

    // 将结果发送回 UI 线程
    auto* resultCopy = new MtuValidationResult(result);
    PostMessageW(g_hWnd, WM_USER_MTU_VALIDATE,
                 result.passed ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(resultCopy));

    delete params;
    return 0;
}

static void StartMtuValidation(uint32_t mtu) {
    if (!TryEnterBusy()) return;
    SetButtonsEnabled(FALSE);

    // 读取 Ping 目标地址
    auto* params = new MtuValidateParams;
    params->mtu = mtu;
    GetWindowTextW(g_hPingAddr, params->target, _countof(params->target));
    if (params->target[0] == L'\0') {
        wcscpy_s(params->target, L"223.5.5.5");
    }

    WriteLog(L"[验证] 正在测试 MTU %lu 是否会导致分片...", mtu);

    HANDLE hThread = CreateThread(nullptr, 0, ValidateThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        WriteLog(L"[错误] 创建验证线程失败");
    }
}

// ============================================================
// MTU 独立测试（仅测试分片，不修改 MTU）
// ============================================================

static DWORD WINAPI MtuTestThreadProc(LPVOID lpParam) {
    auto* params = static_cast<MtuValidateParams*>(lpParam);

    MtuValidationResult result = ValidateMTU(params->target, params->mtu, 4000);

    auto* resultCopy = new MtuValidationResult(result);
    PostMessageW(g_hWnd, WM_USER_MTU_TEST_DONE, 0,
                 reinterpret_cast<LPARAM>(resultCopy));

    delete params;
    return 0;
}

static void RunMtuTest() {
    if (!TryEnterBusy()) return;
    SetButtonsEnabled(FALSE);

    // 读取 MTU 值
    wchar_t mtuBuf[16] = {};
    GetWindowTextW(g_hMtuEdit, mtuBuf, _countof(mtuBuf));
    uint32_t mtu = static_cast<uint32_t>(_wtoi(mtuBuf));
    if (mtu == 0) mtu = 1500;

    // 读取 Ping 目标地址
    auto* params = new MtuValidateParams;
    params->mtu = mtu;
    GetWindowTextW(g_hPingAddr, params->target, _countof(params->target));
    if (params->target[0] == L'\0') {
        wcscpy_s(params->target, L"223.5.5.5");
    }

    WriteLog(L"[测试] 正在测试 MTU %lu 对 %s 的分片情况...", mtu, params->target);

    HANDLE hThread = CreateThread(nullptr, 0, MtuTestThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        WriteLog(L"[错误] 创建 MTU 测试线程失败");
    }
}

static void ApplyMTU(uint32_t mtu) {
    int selIdx = GetSelectedAdapterIndex();
    if (selIdx < 0) {
        MessageBoxW(g_hWnd, L"请先在列表中选择一个网卡。",
                    L"MTU Tool - 提示", MB_ICONINFORMATION);
        return;
    }

    if (mtu < 68 || mtu > 9000) {
        MessageBoxW(g_hWnd, L"MTU 值必须在 68 ~ 9000 之间。",
                    L"MTU Tool - 错误", MB_ICONWARNING);
        return;
    }

    if (!TryEnterBusy()) {
        return;
    }

    SetButtonsEnabled(FALSE);

    // 获取选中网卡的信息
    AcquireSRWLockShared(&g_adaptersLock);
    if (static_cast<size_t>(selIdx) >= g_adapters.size()) {
        ReleaseSRWLockShared(&g_adaptersLock);
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        return;
    }

    auto* params = new MtuTaskParams;
    params->adapterName = g_adapters[static_cast<size_t>(selIdx)].name;
    params->ifIndex     = g_adapters[static_cast<size_t>(selIdx)].ifIndex;
    params->newMTU      = mtu;
    ReleaseSRWLockShared(&g_adaptersLock);

    WriteLog(L"[操作] 正在修改 \"%s\" 的 MTU → %lu ...",
             params->adapterName.c_str(), mtu);

    HANDLE hThread = CreateThread(nullptr, 0, MtuThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        LeaveBusy();
        SetButtonsEnabled(TRUE);
        WriteLog(L"[错误] 创建 MTU 修改线程失败");
    }
}

// 处理 MTU 修改完成消息
static void OnMtuDone(BOOL success, wchar_t* msg) {
    if (msg) {
        AppendLog(msg);
        delete[] msg;
    }

    LeaveBusy();
    SetButtonsEnabled(TRUE);

    // 修改完成后自动刷新列表（后台线程）
    RefreshAdapterList();
}

// ============================================================
// 获取网关测试（后台线程）
// ============================================================

static DWORD WINAPI GatewayThreadProc(LPVOID) {
    // 使用 GetAdaptersAddresses 获取默认网关
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &bufLen);

    auto buf = std::make_unique<BYTE[]>(bufLen);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.get());

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS,
                              nullptr, adapters, &bufLen) != NO_ERROR) {
        auto* msg = new wchar_t[128];
        _snwprintf_s(msg, 128, _TRUNCATE, L"[错误] 无法获取网关信息");
        PostMessageW(g_hWnd, WM_USER_PING_DONE, FALSE, reinterpret_cast<LPARAM>(msg));
        return 0;
    }

    wchar_t gateway[64] = L"未找到";
    bool found = false;

    for (auto* ad = adapters; ad && !found; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp) continue;
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto* gw = ad->FirstGatewayAddress; gw; gw = gw->Next) {
            auto* addr = reinterpret_cast<sockaddr_in*>(gw->Address.lpSockaddr);
            if (addr->sin_family == AF_INET) {
                // 使用 inet_ntoa（winsock2.h 中声明）
                char* ipStr = inet_ntoa(addr->sin_addr);
                MultiByteToWideChar(CP_ACP, 0, ipStr, -1, gateway, _countof(gateway));
                found = true;
                break;
            }
        }
    }

    if (found) {
        // 执行 Ping 测试
        auto* preMsg = new wchar_t[256];
        _snwprintf_s(preMsg, 256, _TRUNCATE, L"[信息] 默认网关: %s，正在 Ping 测试...", gateway);
        PostMessageW(g_hWnd, WM_USER_NETSH_DONE, FALSE, reinterpret_cast<LPARAM>(preMsg));

        bool pingOk = PingTest(gateway, 3000);

        auto* msg = new wchar_t[256];
        _snwprintf_s(msg, 256, _TRUNCATE,
                     pingOk ? L"[成功] 网关 %s Ping 通"
                            : L"[失败] 网关 %s Ping 不通或超时",
                     gateway);
        PostMessageW(g_hWnd, WM_USER_PING_DONE, pingOk ? TRUE : FALSE,
                     reinterpret_cast<LPARAM>(msg));
    } else {
        auto* msg = new wchar_t[128];
        _snwprintf_s(msg, 128, _TRUNCATE, L"[警告] 未找到默认 IPv4 网关");
        PostMessageW(g_hWnd, WM_USER_PING_DONE, FALSE, reinterpret_cast<LPARAM>(msg));
    }

    return 0;
}

static void RunGatewayTest() {
    if (!TryEnterBusy()) return;
    SetButtonsEnabled(FALSE);
    WriteLog(L"[操作] 正在查找默认网关...");

    HANDLE hThread = CreateThread(nullptr, 0, GatewayThreadProc, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        LeaveBusy();
        SetButtonsEnabled(TRUE);
    }
}

// ============================================================
// 恢复默认 MTU（后台线程）
// ============================================================

static DWORD WINAPI RestoreThreadProc(LPVOID) {
    // 遍历注册表备份项，逐个恢复
    // 简化实现：仅恢复当前选中网卡
    // 完整实现需要从注册表读取所有备份并逐一恢复

    // 这里先实现从注册表读取当前选中网卡的备份并恢复
    int selIdx = GetSelectedAdapterIndex();

    std::wstring adapterName;
    NET_IFINDEX ifIndex = 0;

    AcquireSRWLockShared(&g_adaptersLock);
    if (selIdx >= 0 && static_cast<size_t>(selIdx) < g_adapters.size()) {
        adapterName = g_adapters[static_cast<size_t>(selIdx)].name;
        ifIndex     = g_adapters[static_cast<size_t>(selIdx)].ifIndex;
    }
    ReleaseSRWLockShared(&g_adaptersLock);

    if (adapterName.empty()) {
        PostMessageW(g_hWnd, WM_USER_RESTORE_DONE, FALSE,
                     reinterpret_cast<LPARAM>(new wchar_t[64]{ L"[提示] 请先在列表中选择一个网卡" }));
        return 0;
    }

    // 尝试从注册表读取备份
    uint32_t backupMTU = 0;
    if (!RegReadMTUBackup(adapterName, backupMTU)) {
        // 没有备份，恢复为默认 1500
        backupMTU = 1500;
    }

    // 执行恢复
    bool success = SetMTUviaAPI(ifIndex, backupMTU);
    if (!success) {
        success = SetMTUviaNetsh(adapterName, backupMTU);
    }

    wchar_t* msg = new wchar_t[256];
    if (success) {
        _snwprintf_s(msg, 256, _TRUNCATE,
                     L"[成功] 已将 \"%s\" 的 MTU 恢复为 %lu",
                     adapterName.c_str(), backupMTU);
    } else {
        _snwprintf_s(msg, 256, _TRUNCATE,
                     L"[失败] 无法恢复 \"%s\" 的 MTU",
                     adapterName.c_str());
    }
    PostMessageW(g_hWnd, WM_USER_RESTORE_DONE, success ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(msg));

    return 0;
}

static void RunRestoreDefault() {
    if (!TryEnterBusy()) return;
    SetButtonsEnabled(FALSE);
    WriteLog(L"[操作] 正在恢复默认 MTU...");

    HANDLE hThread = CreateThread(nullptr, 0, RestoreThreadProc, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        LeaveBusy();
        SetButtonsEnabled(TRUE);
    }
}

// ============================================================
// Ping 测试（后台线程）
// ============================================================

struct PingTaskParams {
    wchar_t address[256];
};

static DWORD WINAPI PingThreadProc(LPVOID lpParam) {
    auto* params = static_cast<PingTaskParams*>(lpParam);
    wchar_t infoMsg[320];
    _snwprintf_s(infoMsg, _countof(infoMsg), _TRUNCATE,
                 L"[操作] 正在 Ping %s ...", params->address);
    PostMessageW(g_hWnd, WM_USER_LOG_MSG, 0,
                 reinterpret_cast<LPARAM>(_wcsdup(infoMsg)));

    bool success = PingTest(params->address, 3000);

    wchar_t* msg = new wchar_t[320];
    if (success) {
        _snwprintf_s(msg, 320, _TRUNCATE,
                     L"[成功] Ping %s 通过", params->address);
    } else {
        _snwprintf_s(msg, 320, _TRUNCATE,
                     L"[失败] Ping %s 超时或不可达", params->address);
    }
    PostMessageW(g_hWnd, WM_USER_PING_DONE, success ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(msg));

    delete params;
    return 0;
}

static void RunPingTest() {
    if (!TryEnterBusy()) return;
    SetButtonsEnabled(FALSE);

    // 从输入框读取自定义地址（UI 线程安全）
    auto* params = new PingTaskParams;
    GetWindowTextW(g_hPingAddr, params->address, _countof(params->address));
    if (params->address[0] == L'\0') {
        wcscpy_s(params->address, L"223.5.5.5");
    }

    HANDLE hThread = CreateThread(nullptr, 0, PingThreadProc, params, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete params;
        LeaveBusy();
        SetButtonsEnabled(TRUE);
    }
}

// ============================================================
// 窗口消息处理
// ============================================================

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        // ── 最小窗口大小限制 ──
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 680;
            mmi->ptMinTrackSize.y = 450;
            return 0;
        }

        case WM_CREATE:
            OnCreate(hWnd);
            return 0;

        case WM_SIZE:
            OnSize(hWnd, LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_DESTROY:
            SetLogCallback(nullptr);
            if (g_hFont) {
                DeleteObject(g_hFont);
                g_hFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        // ── 命令消息（按钮点击）──
        case WM_COMMAND: {
            WORD id     = LOWORD(wParam);
            WORD notify = HIWORD(wParam);

            // 分割条点击：开始拖拽
            if (id == IDC_SPLITTER && notify == STN_CLICKED) {
                g_splitterDragging = true;
                GetCursorPos(&g_splitterDragStart);
                SetCapture(hWnd);
                return 0;
            }

            if (notify != BN_CLICKED) break;

            switch (id) {
                case IDC_APPLY_BTN: {
                    // 读取输入框中的 MTU 值
                    wchar_t buf[16] = {};
                    GetWindowTextW(g_hMtuEdit, buf, _countof(buf));
                    uint32_t mtu = static_cast<uint32_t>(_wtoi(buf));
                    if (mtu == 0) mtu = 1500;
                    StartMtuValidation(mtu);
                    break;
                }
                case IDC_PRESET_1500:
                    SetWindowTextW(g_hMtuEdit, L"1500");
                    StartMtuValidation(1500);
                    break;
                case IDC_PRESET_1492:
                    SetWindowTextW(g_hMtuEdit, L"1492");
                    StartMtuValidation(1492);
                    break;
                case IDC_PRESET_1472:
                    SetWindowTextW(g_hMtuEdit, L"1472");
                    StartMtuValidation(1472);
                    break;
                case IDC_PRESET_9000:
                    SetWindowTextW(g_hMtuEdit, L"9000");
                    StartMtuValidation(9000);
                    break;
                case IDC_GET_GATEWAY:
                    RunGatewayTest();
                    break;
                case IDC_RESTORE_BTN:
                    RunRestoreDefault();
                    break;
                case IDC_REFRESH_BTN:
                    RefreshAdapterList();
                    break;
                case IDC_PING_BTN:
                    RunPingTest();
                    break;
                case IDC_MTU_TEST_BTN:
                    RunMtuTest();
                    break;
            }
            return 0;
        }

        // ── 自定义消息：网卡枚举完成 ──
        case WM_USER_ENUM_DONE: {
            BOOL success = static_cast<BOOL>(wParam);
            auto* adapters = reinterpret_cast<std::vector<AdapterInfo>*>(lParam);
            OnEnumDone(success, adapters);
            return 0;
        }

        // ── 自定义消息：日志追加 ──
        case WM_USER_LOG_MSG: {
            if (lParam) {
                auto* text = reinterpret_cast<wchar_t*>(lParam);
                AppendLog(text);
                delete[] text;
            }
            return 0;
        }

        // ── 自定义消息：MTU 修改完成 ──
        case WM_USER_MTU_DONE: {
            auto* msg = reinterpret_cast<wchar_t*>(lParam);
            OnMtuDone(static_cast<BOOL>(wParam), msg);
            return 0;
        }

        // ── 自定义消息：Ping 测试完成 ──
        case WM_USER_PING_DONE: {
            if (lParam) {
                auto* msg = reinterpret_cast<wchar_t*>(lParam);
                AppendLog(msg);
                delete[] msg;
            }
            LeaveBusy();
            SetButtonsEnabled(TRUE);
            return 0;
        }

        // ── 自定义消息：Netsh 操作完成（日志）──
        case WM_USER_NETSH_DONE: {
            if (lParam) {
                auto* msg = reinterpret_cast<wchar_t*>(lParam);
                AppendLog(msg);
                delete[] msg;
            }
            return 0;
        }

        // ── 自定义消息：恢复完成 ──
        case WM_USER_RESTORE_DONE: {
            if (lParam) {
                auto* msg = reinterpret_cast<wchar_t*>(lParam);
                AppendLog(msg);
                delete[] msg;
            }
            LeaveBusy();
            SetButtonsEnabled(TRUE);
            // 自动刷新列表
            RefreshAdapterList();
            return 0;
        }

        // ── 自定义消息：MTU 分片验证完成 ──
        case WM_USER_MTU_VALIDATE: {
            auto* result = reinterpret_cast<MtuValidationResult*>(lParam);
            if (result) {
                AppendLog(result->message.c_str());
                wchar_t askMsg[512];
                if (result->passed) {
                    // 验证通过，询问用户确认
                    _snwprintf_s(askMsg, _countof(askMsg), _TRUNCATE,
                                 L"MTU 分片验证通过:\r\n\r\n%s\r\n\r\n确认修改 MTU 为 %lu？",
                                 result->message.c_str(), result->testedMTU);
                    int choice = MessageBoxW(g_hWnd, askMsg,
                                             L"MTU Tool - 确认修改",
                                             MB_OKCANCEL | MB_ICONINFORMATION);
                    if (choice == IDOK) {
                        LeaveBusy();
                        ApplyMTU(result->testedMTU);
                    } else {
                        LeaveBusy();
                        SetButtonsEnabled(TRUE);
                        WriteLog(L"[信息] 用户取消了 MTU 修改");
                    }
                } else {
                    // 验证未通过，警告用户
                    _snwprintf_s(askMsg, _countof(askMsg), _TRUNCATE,
                                 L"MTU 分片验证未通过:\r\n\r\n%s\r\n\r\n是否仍然修改 MTU 为 %lu？",
                                 result->message.c_str(), result->testedMTU);
                    int choice = MessageBoxW(g_hWnd, askMsg,
                                             L"MTU Tool - 验证警告",
                                             MB_YESNO | MB_ICONWARNING);
                    if (choice == IDYES) {
                        LeaveBusy();
                        ApplyMTU(result->testedMTU);
                    } else {
                        LeaveBusy();
                        SetButtonsEnabled(TRUE);
                        WriteLog(L"[信息] 用户取消了 MTU 修改");
                    }
                }
                delete result;
            }
            return 0;
        }

        // ── 自定义消息：MTU 分片独立测试完成 ──
        case WM_USER_MTU_TEST_DONE: {
            auto* result = reinterpret_cast<MtuValidationResult*>(lParam);
            if (result) {
                AppendLog(L"──────────────────────────────");
                AppendLog(result->message.c_str());
                AppendLog(L"──────────────────────────────");
                delete result;
            }
            LeaveBusy();
            SetButtonsEnabled(TRUE);
            return 0;
        }

        // ── Enter 键在 MTU 输入框中触发应用 ──
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) {
                HWND hFocus = GetFocus();
                if (hFocus == g_hMtuEdit) {
                    SendMessageW(hWnd, WM_COMMAND,
                                 MAKEWPARAM(IDC_APPLY_BTN, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(GetDlgItem(hWnd, IDC_APPLY_BTN)));
                }
            }
            break;
        }

        // ── 分割条拖拽 ──
        case WM_MOUSEMOVE:
            if (g_splitterDragging) {
                POINT pt;
                GetCursorPos(&pt);
                int dy = g_splitterDragStart.y - pt.y;
                g_splitterDragStart = pt;
                g_logHeight += dy;

                RECT rc;
                GetClientRect(hWnd, &rc);
                OnSize(hWnd, rc.right - rc.left, rc.bottom - rc.top);
            }
            break;

        case WM_LBUTTONUP:
            if (g_splitterDragging) {
                g_splitterDragging = false;
                ReleaseCapture();
            }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hWnd, &pt);
                HWND hChild = ChildWindowFromPoint(hWnd, pt);
                if (hChild == g_hSplitter) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
