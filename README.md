# MTU Tool

Windows 本地网卡 MTU 值修改工具 — 纯 Win32 原生实现，零运行时依赖，体积极致小巧。

## 功能

- **网卡枚举**：列出所有 IPv4 网卡，显示名称、当前 MTU、MAC 地址、连接状态
- **MTU 修改**：支持手动输入 + 常用预设一键设置
  - `1500 标准以太网` / `1492 PPPoE` / `1472 VPN` / `9000 巨型帧`
- **分片验证**：修改前自动通过 `ping -f -l` 检测目标 MTU 是否导致分片，并给出建议
- **MTU 独立测试**：不修改系统设置，仅测试指定 MTU 值的分片情况
- **Ping 连通性测试**：支持自定义目标地址
- **网关检测**：自动查找默认网关并 Ping 测试
- **恢复默认**：从注册表备份恢复或重置为 1500
- **日志面板**：带时间戳的实时操作日志
- **可拖拽分割条**：列表面板和日志面板高度可自由调整

## 截图

```
┌──────────────────────────────────────────────────────┐
│  本地网卡 MTU 修改工具 v2.0                           │
├──────────────────────────────────────────────────────┤
│  网卡名称          │ 当前MTU │ MAC地址       │ 状态  │
│  ─────────────────────────────────────────────────── │
│  Ethernet0         │  1500   │ XX-XX-XX-XX │ 已连接│
│  Wi-Fi             │  1492   │ XX-XX-XX-XX │ 已连接│
│                                                     │
│  修改MTU: [____] [应用]   预设: [1500] [1492] [1472] [9000] │
│  Ping目标: [____] [Ping测试] [MTU测试] │ [获取网关] [恢复默认] [刷新列表] │
│  ─────────────── 拖拽分割条 ───────────────           │
│  12:00:00  MTU Tool v2.0 已启动                      │
│  12:00:01  列表刷新完成，共 2 个网卡                   │
└──────────────────────────────────────────────────────┘
```

## 构建

### 前置条件

- Windows 10/11 x64
- [Visual Studio 2022 BuildTools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)（含 MSVC v143 和 Windows 10/11 SDK）
- 或完整 Visual Studio 2022

### 快速构建

```powershell
# 使用 build.ps1 直接调用 MSVC 工具链
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

产物：`mtu-tool.exe`（约 185 KB）

### CMake 构建（可选）

```powershell
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### 编译选项

| 选项 | 说明 |
|------|------|
| `/O2 /GS- /Gy` | 体积+速度优化 |
| `/MT` | 静态链接 CRT，无需 VC 运行时 |
| `/EHs-c- /GR-` | 禁用 C++ 异常和 RTTI，减小体积 |
| `/utf-8` | 源文件 UTF-8 编码 |
| `_WIN32_WINNT=0x0601` | 目标 Windows 7+ |

## 使用

1. **以管理员身份运行** `mtu-tool.exe`（程序会自动检测并提权）
2. 在列表中选择目标网卡
3. 输入 MTU 值或点击预设按钮
4. 程序自动验证分片 → 弹窗确认 → 应用修改
5. 通过日志面板查看操作结果

## 技术架构

```
src/
├── main.cpp      # 入口：UAC 提权、Winsock 初始化、消息循环
├── ui.h/cpp      # Win32 窗口、控件布局、消息处理 (WndProc)
├── network.h/cpp # 网卡枚举、MTU 修改 (API + netsh)、Ping 测试
├── utils.h/cpp   # 字符串转换、注册表读写、管理员检测、日志
res/
├── app.rc        # 资源文件（图标、Manifest、版本信息）
├── app.manifest  # UAC requireAdministrator 声明
scripts/
└── gen_icon.ps1  # 图标生成脚本
```

### 关键技术点

- 所有耗时操作（网络枚举、Ping、MTU 修改）在后台线程异步执行，通过 `PostMessage` + 自定义 `WM_USER` 消息与 UI 线程通信，窗口不卡死
- 多线程共享数据使用 `SRWLOCK` 读写锁保护
- MTU 修改优先使用 `SetIpInterfaceEntry` API，失败时回退 `netsh` 命令行
- 分片验证通过 `ping -f -l <payload>` 检测 DF 标志
- 中英文系统输出兼容（GBK 编码检测 → UTF-8 转换）

## 许可

MIT