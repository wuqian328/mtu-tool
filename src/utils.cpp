// ── utils.cpp ────────────────────────────────────────
//   字符串转换、注册表读写、管理员检测、日志记录
// ─────────────────────────────────────────────────────

#include "utils.h"
#include <cstdio>
#include <cstdarg>
#include <shellapi.h>

// ── 全局日志回调 ─────────────────────────────────────
//   UI 层调用 SetLogCallback 注册；WriteLog 内部调用
static LogCallback g_pLogCallback = nullptr;

void SetLogCallback(LogCallback cb) {
    g_pLogCallback = cb;
}

// ============================================================
// 字符串转换
// ============================================================

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return L"";
    }
    // 先获取目标缓冲区大小（含结尾 '\0'）
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], len);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return "";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return "";
    }
    std::string result(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// ============================================================
// 管理员权限检测与提权
// ============================================================

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID  adminGroup = nullptr;

    // 构建 Administrators 组的 SID
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        // 检查当前 token 是否属于 Administrators 组
        if (!CheckTokenMembership(nullptr, adminGroup, &isAdmin)) {
            isAdmin = FALSE;
        }
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

void RelaunchAsAdmin() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // 使用 ShellExecuteW("runas") 触发 UAC 提权对话框
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";          // 以管理员身份运行
    sei.lpFile       = exePath;
    sei.nShow        = SW_SHOWNORMAL;
    sei.fMask        = SEE_MASK_DEFAULT;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        // ERROR_CANCELLED (1223) 表示用户拒绝了 UAC 提权
        if (err != ERROR_CANCELLED) {
            wchar_t msg[256];
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                L"提权启动失败 (错误码: %lu)", err);
            MessageBoxW(nullptr, msg, L"MTU Tool - 错误", MB_ICONERROR);
        }
    }
}

// ============================================================
// 注册表备份操作
// ============================================================

bool RegReadMTUBackup(const std::wstring& adapterName, uint32_t& result) {
    HKEY hKey = nullptr;
    const std::wstring subKey = L"Software\\MTUTool\\Backup";

    LONG lr = RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(),
                             0, KEY_READ, &hKey);
    if (lr != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = REG_DWORD;
    DWORD data = 0;
    DWORD size = sizeof(data);

    lr = RegQueryValueExW(hKey, adapterName.c_str(), nullptr,
                          &type, reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(hKey);

    if (lr == ERROR_SUCCESS && type == REG_DWORD) {
        result = static_cast<uint32_t>(data);
        return true;
    }
    return false;
}

bool RegWriteMTUBackup(const std::wstring& adapterName, uint32_t mtu) {
    HKEY hKey = nullptr;
    const std::wstring subKey = L"Software\\MTUTool\\Backup";

    // 创建或打开注册表键（不存在时自动创建）
    LONG lr = RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(),
                               0, nullptr, REG_OPTION_NON_VOLATILE,
                               KEY_WRITE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS) {
        return false;
    }

    DWORD data = static_cast<DWORD>(mtu);
    lr = RegSetValueExW(hKey, adapterName.c_str(), 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(hKey);

    return (lr == ERROR_SUCCESS);
}

// ============================================================
// 日志记录
// ============================================================

void WriteLog(const wchar_t* fmt, ...) {
    if (!g_pLogCallback) {
        return;
    }

    wchar_t buf[2048] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    g_pLogCallback(buf);
}
