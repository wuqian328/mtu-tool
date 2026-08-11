#pragma once

// ── UI 模块 ──────────────────────────────────────────
//   Win32 窗口创建、控件布局、消息处理 (WndProc)
// ─────────────────────────────────────────────────────

#include <winsock2.h>   // 必须在 windows.h 之前，避免与 winsock.h 冲突
#include <windows.h>
#include <commctrl.h>

// ============================================================
// 控件 ID 定义
// ============================================================
#define IDC_LISTVIEW      1001    // 网卡列表 ListView
#define IDC_MTU_EDIT      1002    // MTU 输入框
#define IDC_APPLY_BTN     1003    // 应用按钮
#define IDC_PRESET_1500   1004    // 预设 1500
#define IDC_PRESET_1492   1005    // 预设 1492
#define IDC_PRESET_1472   1006    // 预设 1472
#define IDC_PRESET_9000   1007    // 预设 9000
#define IDC_GET_GATEWAY   1008    // 获取网关
#define IDC_RESTORE_BTN   1009    // 恢复默认 MTU
#define IDC_REFRESH_BTN   1010    // 刷新列表
#define IDC_PING_BTN      1011    // Ping 测试
#define IDC_LOG_EDIT      1012    // 日志面板
#define IDC_MTU_LABEL     1013    // "修改MTU:" 标签
#define IDC_PRESET_LABEL  1014    // "预设:" 标签
#define IDC_PING_ADDR     1015    // Ping 目标地址输入框
#define IDC_PING_LABEL    1016    // "Ping目标:" 标签
#define IDC_MTU_TEST_BTN  1017    // MTU 分片测试按钮
#define IDC_SPLITTER      1018    // 列表/日志分割条

// ============================================================
// 自定义窗口消息
// ============================================================
// 后台线程完成网卡枚举后，通过此消息将结果发送到 UI 线程
//   WPARAM: 未使用
//   LPARAM: AdapterInfo 向量指针（UI 线程处理后释放）
#define WM_USER_ENUM_DONE       (WM_USER + 100)

// 后台线程追加日志行（线程安全）
//   WPARAM: 未使用
//   LPARAM: wchar_t* 日志文本指针（UI 线程处理后释放）
#define WM_USER_LOG_MSG         (WM_USER + 101)

// MTU 修改完成通知
//   WPARAM: BOOL 成功/失败
//   LPARAM: wchar_t* 结果消息指针
#define WM_USER_MTU_DONE        (WM_USER + 102)

// Ping 测试完成通知
//   WPARAM: BOOL 成功/失败
//   LPARAM: wchar_t* 结果消息指针
#define WM_USER_PING_DONE       (WM_USER + 103)

// Netsh 回退执行结果
//   WPARAM: BOOL 成功/失败
//   LPARAM: wchar_t* 输出文本指针
#define WM_USER_NETSH_DONE      (WM_USER + 104)

// 恢复默认 MTU 操作完成
//   WPARAM: BOOL 成功/失败
//   LPARAM: wchar_t* 结果消息指针
#define WM_USER_RESTORE_DONE    (WM_USER + 105)

// MTU 分片验证完成
//   WPARAM: BOOL 验证通过 (TRUE) / 未通过 (FALSE)
//   LPARAM: MtuValidateResult* 指针（UI 线程负责 delete）
#define WM_USER_MTU_VALIDATE     (WM_USER + 106)

// MTU 分片独立测试完成（仅记录日志，不弹窗）
//   WPARAM: 未使用
//   LPARAM: MtuValidationResult* 指针（UI 线程负责 delete）
#define WM_USER_MTU_TEST_DONE    (WM_USER + 107)

// ============================================================
// 窗口管理
// ============================================================

// 注册窗口类并创建主窗口
//   参数 hInstance : 应用程序实例句柄
//   返回 true 表示创建成功
bool CreateMainWindow(HINSTANCE hInstance);

// 进入消息循环（阻塞直到窗口关闭）
int RunMessageLoop();

// 获取主窗口句柄（供后台线程发送消息使用）
HWND GetMainWindow();

// 获取 ListView 控件句柄
HWND GetAdapterListView();
