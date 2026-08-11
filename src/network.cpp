// ── network.cpp ─────────────────────────────────────
//   网卡枚举、MTU 修改 (SetIpInterfaceEntry + netsh 回退)、Ping 测试
// ─────────────────────────────────────────────────────

#include "network.h"
#include "utils.h"

#include <icmpapi.h>     // IcmpCreateFile / IcmpSendEcho
#include <ws2tcpip.h>    // addrinfo / getaddrinfo

#include <cstdio>
#include <memory>

// 链接声明（部分 API 不在导入库中需显式声明）
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// ============================================================
// 网卡枚举
// ============================================================

bool EnumerateAdapters(std::vector<AdapterInfo>& outAdapters) {
    outAdapters.clear();

    // 第一次调用：获取所需缓冲区大小
    ULONG bufLen = 0;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;   // 不包含网关等额外信息，加快速度
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufLen);
    if (ret != ERROR_BUFFER_OVERFLOW) {
        WriteLog(L"[错误] GetAdaptersAddresses 获取缓冲区大小失败: %lu", ret);
        return false;
    }

    // 分配缓冲区
    auto buf = std::make_unique<BYTE[]>(bufLen);
    auto* pAdapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.get());

    ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAdapter, &bufLen);
    if (ret != NO_ERROR) {
        WriteLog(L"[错误] GetAdaptersAddresses 枚举失败: %lu", ret);
        return false;
    }

    // 遍历网卡链表
    int adapterCount = 0;
    for (auto* ad = pAdapter; ad != nullptr; ad = ad->Next) {
        // 过滤条件：跳过回环和隧道接口
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        AdapterInfo info = {};

        // ── 名称 ──
        info.name = (ad->FriendlyName && ad->FriendlyName[0])
                        ? ad->FriendlyName
                        : (ad->Description ? ad->Description : L"(未知网卡)");

        info.description = ad->Description ? ad->Description : L"";

        // ── MAC 地址 ──
        if (ad->PhysicalAddressLength > 0) {
            wchar_t macStr[18] = {};
            wchar_t* p = macStr;
            for (ULONG i = 0; i < ad->PhysicalAddressLength && i < 6; i++) {
                if (i > 0) {
                    *p++ = L'-';
                }
                p += swprintf_s(p, 4, L"%02X", ad->PhysicalAddress[i]);
            }
            info.mac = macStr;
        } else {
            info.mac = L"N/A";
        }

        // ── 连接状态 ──
        info.connected = (ad->OperStatus == IfOperStatusUp);

        // ── 接口索引 ──
        // GetAdaptersAddresses 的 IfIndex 用于 MIB_IPINTERFACE_ROW.InterfaceIndex
        info.ifIndex = ad->IfIndex;

        // ── 获取当前 MTU ──
        // 优先使用 GetIpInterfaceEntry 获取 IP 层 MTU
        MIB_IPINTERFACE_ROW ipRow = {};
        ipRow.Family = AF_INET;
        ipRow.InterfaceIndex = info.ifIndex;

        if (GetIpInterfaceEntry(&ipRow) == NO_ERROR) {
            info.mtu = ipRow.NlMtu;
        } else {
            // 回退：使用 GetAdaptersAddresses 返回的链路层 MTU
            info.mtu = ad->Mtu;
        }

        outAdapters.push_back(info);
        adapterCount++;
    }

    WriteLog(L"[信息] 共枚举 %d 个 IPv4 网卡", adapterCount);
    return true;
}

// ============================================================
// MTU 修改 ── 优先方案: SetIpInterfaceEntry
// ============================================================

bool SetMTUviaAPI(NET_IFINDEX ifIndex, uint32_t newMTU) {
    // 第 1 步：先获取当前接口信息（填充除 NlMtu 外的所有字段）
    MIB_IPINTERFACE_ROW row = {};
    row.Family = AF_INET;
    row.InterfaceIndex = ifIndex;

    DWORD ret = GetIpInterfaceEntry(&row);
    if (ret != NO_ERROR) {
        WriteLog(L"[错误] GetIpInterfaceEntry 失败 (IfIndex=%lu, 错误码=%lu)", ifIndex, ret);
        SetLastError(ret);
        return false;
    }

    uint32_t oldMTU = row.NlMtu;

    // 第 2 步：修改 MTU 字段
    row.NlMtu = newMTU;
    // 同时确保 SitePrefixLength 等字段有效（保持原值即可）

    // 第 3 步：调用 SetIpInterfaceEntry 提交修改
    ret = SetIpInterfaceEntry(&row);
    if (ret != NO_ERROR) {
        WriteLog(L"[错误] SetIpInterfaceEntry 失败 (IfIndex=%lu, MTU=%lu, 错误码=%lu)",
                 ifIndex, newMTU, ret);
        SetLastError(ret);
        return false;
    }

    WriteLog(L"[成功] SetIpInterfaceEntry: MTU %lu → %lu (IfIndex=%lu)",
             oldMTU, newMTU, ifIndex);
    return true;
}

// ============================================================
// MTU 修改 ── 回退方案: netsh 命令行
// ============================================================

// 匿名管道辅助函数：读取进程 stdout 输出到字符串
static std::wstring ReadProcessOutput(HANDLE hReadPipe) {
    std::wstring output;
    char buf[4096];
    DWORD bytesRead = 0;

    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        // netsh 输出为 GBK 编码，需要转为宽字符串
        // 先尝试直接转宽字符串（假定系统本地编码为 GBK/CP936）
        int wlen = MultiByteToWideChar(CP_ACP, 0, buf, static_cast<int>(bytesRead), nullptr, 0);
        if (wlen > 0) {
            std::wstring tmp(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_ACP, 0, buf, static_cast<int>(bytesRead), &tmp[0], wlen);
            output += tmp;
        }
    }
    return output;
}

bool SetMTUviaNetsh(const std::wstring& adapterName, uint32_t newMTU) {
    // 构建命令行: netsh interface ipv4 set subinterface "<name>" mtu=<值> store=persistent
    wchar_t cmdLine[512] = {};
    _snwprintf_s(cmdLine, _countof(cmdLine), _TRUNCATE,
                 L"netsh interface ipv4 set subinterface \"%s\" mtu=%lu store=persistent",
                 adapterName.c_str(), newMTU);

    WriteLog(L"[执行] %s", cmdLine);

    // ── 创建匿名管道捕获输出 ──
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;     // 管道句柄可被子进程继承
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe  = nullptr;
    HANDLE hWritePipe = nullptr;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        WriteLog(L"[错误] CreatePipe 失败: %lu", GetLastError());
        return false;
    }

    // 确保读端不被子进程继承（只需写端被继承）
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // ── 创建子进程 ──
    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput  = hWritePipe;   // stdout 重定向到管道
    si.hStdError   = hWritePipe;   // stderr 也重定向到管道
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;      // 隐藏控制台窗口

    PROCESS_INFORMATION pi = {};

    BOOL created = CreateProcessW(
        nullptr,                // 应用程序名称（从命令行解析）
        cmdLine,                // 命令行
        nullptr,                // 进程安全属性
        nullptr,                // 线程安全属性
        TRUE,                   // 继承句柄（管道需要）
        CREATE_NO_WINDOW,       // 不创建新控制台
        nullptr,                // 环境变量
        nullptr,                // 工作目录
        &si,
        &pi
    );

    // 关闭写端（子进程已继承，父进程不需要）
    CloseHandle(hWritePipe);

    if (!created) {
        DWORD err = GetLastError();
        WriteLog(L"[错误] CreateProcessW 失败: %lu", err);
        CloseHandle(hReadPipe);
        return false;
    }

    // ── 等待子进程结束 ──
    WaitForSingleObject(pi.hProcess, 15000);   // 超时 15 秒

    // 读取输出
    std::wstring output = ReadProcessOutput(hReadPipe);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // ── 检查执行结果 ──
    // netsh 成功时通常输出 "确定。"
    if (output.find(L"确定") != std::wstring::npos ||
        output.find(L"Ok")  != std::wstring::npos) {
        WriteLog(L"[成功] netsh: %s", output.c_str());
        return true;
    }

    WriteLog(L"[失败] netsh 输出: %s", output.c_str());
    return false;
}

// ============================================================
// Ping 连通性测试
// ============================================================

bool PingTest(const std::wstring& address, uint32_t timeout) {
    // 打开 ICMP 句柄
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        WriteLog(L"[错误] IcmpCreateFile 失败: %lu", GetLastError());
        return false;
    }

    // ── 解析目标地址 ──
    ULONG ipAddr = INADDR_NONE;

    // 先尝试作为点分十进制 IP 解析
    ipAddr = inet_addr(WideToUtf8(address).c_str());

    if (ipAddr == INADDR_NONE) {
        // DNS 解析
        addrinfo hints = {};
        hints.ai_family = AF_INET;

        addrinfo* result = nullptr;
        std::string narrow = WideToUtf8(address);
        if (getaddrinfo(narrow.c_str(), nullptr, &hints, &result) != 0) {
            WriteLog(L"[错误] 无法解析地址: %s", address.c_str());
            IcmpCloseHandle(hIcmp);
            return false;
        }

        ipAddr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(result);
    }

    // ── 发送 ICMP Echo Request ──
    constexpr DWORD DATA_SIZE = 32;
    BYTE sendData[DATA_SIZE] = {};
    // 填充发送数据（Ping 常用填充）
    for (DWORD i = 0; i < DATA_SIZE; i++) {
        sendData[i] = static_cast<BYTE>('a' + (i % 26));
    }

    // 计算回复缓冲区大小
    constexpr DWORD replySize = sizeof(ICMP_ECHO_REPLY) + DATA_SIZE + 8;
    auto replyBuf = std::make_unique<BYTE[]>(replySize);

    DWORD dwRet = IcmpSendEcho(
        hIcmp,
        ipAddr,
        sendData,
        DATA_SIZE,
        nullptr,                   // IP 选项（不需要）
        replyBuf.get(),
        replySize,
        timeout
    );

    IcmpCloseHandle(hIcmp);

    if (dwRet == 0) {
        WriteLog(L"[Ping] %s - 请求超时或无响应 (错误码: %lu)",
                 address.c_str(), GetLastError());
        return false;
    }

    auto* echoReply = reinterpret_cast<PICMP_ECHO_REPLY>(replyBuf.get());
    if (echoReply->Status != IP_SUCCESS) {
        WriteLog(L"[Ping] %s - 状态异常 (Status=%lu)", address.c_str(), echoReply->Status);
        return false;
    }

    WriteLog(L"[Ping] %s - 成功 (RTT=%lums, TTL=%lu)",
             address.c_str(), echoReply->RoundTripTime, echoReply->Options.Ttl);
    return true;
}

// ============================================================
// MTU 分片验证 (ping -f -l)
// ============================================================

MtuValidationResult ValidateMTU(const std::wstring& target, uint32_t mtu, uint32_t timeout) {
    MtuValidationResult result = {};
    result.testedMTU = mtu;

    // 有效载荷 = MTU - 28（20 IP 头 + 8 ICMP 头）
    if (mtu < 68) {
        result.passed   = false;
        result.executed = false;
        result.message  = L"MTU 值过小（< 68），无法验证";
        return result;
    }

    uint32_t payload = mtu - 28;

    // 构建命令行: ping -n 1 -f -l <payload> <target>
    wchar_t cmdLine[512];
    _snwprintf_s(cmdLine, _countof(cmdLine), _TRUNCATE,
                 L"ping -n 1 -f -l %lu %s", payload, target.c_str());

    WriteLog(L"[验证] %s", cmdLine);

    // ── 创建匿名管道 ──
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe  = nullptr;
    HANDLE hWritePipe = nullptr;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        result.passed   = false;
        result.executed = false;
        result.message  = L"无法创建管道执行验证命令";
        return result;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // ── 创建子进程 ──
    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput  = hWritePipe;
    si.hStdError   = hWritePipe;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    BOOL created = CreateProcessW(
        nullptr, cmdLine, nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
        &si, &pi
    );

    CloseHandle(hWritePipe);

    if (!created) {
        CloseHandle(hReadPipe);
        result.passed   = false;
        result.executed = false;
        result.message  = L"无法启动 ping 命令进行验证";
        return result;
    }

    // ── 等待子进程结束 ──
    WaitForSingleObject(pi.hProcess, timeout);

    // 读取输出
    std::wstring output = ReadProcessOutput(hReadPipe);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.executed = true;

    // ── 解析输出 ──
    // 中文系统: "需要拆分数据包但是设置 DF" 或 "Packet needs to be fragmented but DF set"
    // 成功: "来自 x.x.x.x 的回复" 或 "Reply from"
    bool fragmented = false;
    bool replied    = false;

    if (output.find(L"需要拆分") != std::wstring::npos ||
        output.find(L"fragmented") != std::wstring::npos) {
        fragmented = true;
    }

    if (output.find(L"的回复") != std::wstring::npos ||
        output.find(L"Reply from") != std::wstring::npos) {
        replied = true;
    }

    if (fragmented) {
        result.passed  = false;
        result.message = L"MTU " + std::to_wstring(mtu) +
                         L" 过大，路径中存在更小的 MTU，数据包需要分片。\r\n"
                         L"建议尝试较小的 MTU 值（如 1472 或 1450）。";
    } else if (replied) {
        result.passed  = true;
        result.message = L"MTU " + std::to_wstring(mtu) +
                         L" 验证通过，不会导致分片。";
    } else {
        // 未收到回复也未报分片（可能超时或目标不可达）
        result.passed  = false;
        result.message = L"MTU " + std::to_wstring(mtu) +
                         L" 验证无响应（目标可能不可达或超时）。\r\n"
                         L"无法确定是否会分片，建议谨慎操作。";
    }

    WriteLog(L"[验证结果] %s", result.message.c_str());
    return result;
}
