#pragma once

// ── 公共头文件 ───────────────────────────────────────
//   字符串转换、注册表读写、管理员检测、日志记录
// ─────────────────────────────────────────────────────

#include <winsock2.h>   // 必须在 windows.h 之前，避免与 winsock.h 冲突
#include <windows.h>
#include <string>
#include <cstdint>

// ============================================================
// 字符串转换
// ============================================================

// UTF-8 (char) → 宽字符串 (wchar_t)
std::wstring Utf8ToWide(const std::string& utf8);

// 宽字符串 (wchar_t) → UTF-8 (char)
std::string WideToUtf8(const std::wstring& wide);

// ============================================================
// 管理员权限检测与提权
// ============================================================

// 通过 CheckTokenMembership 检测当前进程是否以管理员权限运行
bool IsRunningAsAdmin();

// 通过 ShellExecuteW("runas") 以管理员权限重新启动当前程序
// 调用后当前进程应立即退出（新进程已启动）
void RelaunchAsAdmin();

// ============================================================
// 注册表备份操作
// ============================================================

// 从 HKCU\Software\MTUTool\Backup\{adapterName} 读取备份的 MTU 值
// 返回 true 表示读取成功，result 为读取到的值
bool RegReadMTUBackup(const std::wstring& adapterName, uint32_t& result);

// 将 MTU 值写入 HKCU\Software\MTUTool\Backup\{adapterName}
// 返回 true 表示写入成功
bool RegWriteMTUBackup(const std::wstring& adapterName, uint32_t mtu);

// ============================================================
// 日志系统
// ============================================================

// 日志回调函数类型：由 UI 层注册，负责将日志文本追加到日志面板
using LogCallback = void(*)(const wchar_t* msg);

// 设置全局日志回调（应在 UI 初始化时调用）
void SetLogCallback(LogCallback cb);

// 格式化输出日志（线程安全，可通过 SendMessage 异步展示）
// 内部调用已注册的 LogCallback
void WriteLog(const wchar_t* fmt, ...);
