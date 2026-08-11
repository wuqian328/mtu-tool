#pragma once

// ── 网络操作模块 ─────────────────────────────────────
//   网卡枚举、MTU 修改、Ping 测试
//   _WIN32_WINNT / NTDDI_VERSION 通过编译标志 /D 定义
// ─────────────────────────────────────────────────────

#include <winsock2.h>   // 必须在 windows.h 之前，避免与 winsock.h 冲突
#include <windows.h>
#include <ws2def.h>     // netioapi.h 的前置依赖（需在 iphlpapi.h 之前）
#include <ws2ipdef.h>   // netioapi.h 的前置依赖
#include <iphlpapi.h>   // 内含 netioapi.h (GetIpInterfaceEntry / SetIpInterfaceEntry)
#include <string>
#include <vector>
#include <cstdint>

// ============================================================
// 网卡信息结构体
// ============================================================
struct AdapterInfo {
    std::wstring name;          // 网卡友好名称 (FriendlyName)
    std::wstring description;   // 网卡描述 (Description)
    std::wstring mac;           // MAC 地址，格式: XX-XX-XX-XX-XX-XX
    uint32_t    mtu;            // 当前 MTU 值（0 表示无法获取）
    bool        connected;      // 操作状态是否为 Up
    NET_IFINDEX ifIndex;        // 网络接口索引（用于 SetIpInterfaceEntry）
};

// ============================================================
// 网卡枚举（同步调用，调用者应放在后台线程中执行）
// ============================================================

// 枚举所有 IPv4 网卡，过滤回环接口
// 返回 true 表示枚举成功
bool EnumerateAdapters(std::vector<AdapterInfo>& outAdapters);

// ============================================================
// MTU 修改
// ============================================================

// 优先方案: 通过 SetIpInterfaceEntry (netioapi) 修改 MTU
//   参数 ifIndex : 网络接口索引（来自 AdapterInfo::ifIndex）
//   参数 newMTU  : 目标 MTU 值
//   成功返回 true，失败时通过 GetLastError() 获取详细错误码
bool SetMTUviaAPI(NET_IFINDEX ifIndex, uint32_t newMTU);

// 回退方案: 通过 netsh 命令行修改 MTU
//   参数 ifIndex    : 网络接口索引（用于 netsh 命令）
//   参数 newMTU      : 目标 MTU 值
//   成功返回 true，失败时可通过 GetLastError 获取 CreateProcess 错误
bool SetMTUviaNetsh(NET_IFINDEX ifIndex, uint32_t newMTU);

// ============================================================
// 连通性测试
// ============================================================

// 通过 IcmpSendEcho (ICMP Echo Request) 测试目标地址的连通性
//   参数 address : 目标 IP 地址或域名
//   参数 timeout : 超时时间（毫秒），默认 3000
//   成功返回 true（至少收到一个响应）
bool PingTest(const std::wstring& address, uint32_t timeout = 3000);

// ============================================================
// MTU 分片验证
// ============================================================

// 验证结果结构体
struct MtuValidationResult {
    bool     passed;        // true = 不会分片，可以安全使用该 MTU
    bool     executed;      // true = 验证命令已执行（false 表示命令执行失败）
    uint32_t testedMTU;     // 测试的 MTU 值
    std::wstring message;   // 人类可读的验证结果消息
};

// 通过 ping -f -l <payload> 命令测试目标 MTU 是否会导致分片
//   参数 target   : Ping 目标地址（IP 或域名）
//   参数 mtu      : 要测试的 MTU 值
//   参数 timeout  : 超时时间（毫秒），默认 3000
//   有效载荷 = mtu - 28（20 IP 头 + 8 ICMP 头）
MtuValidationResult ValidateMTU(const std::wstring& target, uint32_t mtu, uint32_t timeout = 3000);
