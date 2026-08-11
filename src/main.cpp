// ── main.cpp ────────────────────────────────────────
//   程序入口：UAC 管理员权限检测、提权、WinMain 初始化
//   - 非管理员权限时自动通过 ShellExecute("runas") 提权
//   - Manifest 中已声明 requireAdministrator，正常情况下
//     Windows 会在启动时自动弹出 UAC 对话框
// ─────────────────────────────────────────────────────

#include <winsock2.h>  // 必须在所有其他头文件之前，避免与 winsock.h 冲突
#include <windows.h>
#include "utils.h"
#include "ui.h"

// ============================================================
// WinMain 入口
// ============================================================

int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       nCmdShow)
{
    // 禁用未引用参数警告
    UNREFERENCED_PARAMETER(nCmdShow);

    // ── 第 1 步：管理员权限检测 ──
    //   Manifest 中已设置 requireAdministrator，正常启动时 Windows
    //   会自动弹出 UAC 对话框要求提权。这是第一道防线。
    //
    //   如果在某些情况下（如通过 CreateProcess 调用）绕过了 Manifest，
    //   这里做第二道防线：手动检测并触发提权。
    if (!IsRunningAsAdmin()) {
        RelaunchAsAdmin();
        // 提权启动新进程后，当前非管理员进程退出
        return 0;
    }

    // ── 第 2 步：初始化 Winsock（Ping 等功能需要）──
    WSADATA wsaData = {};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // ── 第 3 步：创建主窗口 ──
    if (!CreateMainWindow(hInstance)) {
        MessageBoxW(nullptr,
                    L"无法创建主窗口。",
                    L"MTU Tool - 致命错误",
                    MB_ICONERROR | MB_OK);
        WSACleanup();
        return 1;
    }

    // ── 第 4 步：进入消息循环 ──
    int exitCode = RunMessageLoop();

    // ── 清理 ──
    WSACleanup();

    return exitCode;
}
