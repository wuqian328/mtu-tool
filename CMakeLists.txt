cmake_minimum_required(VERSION 3.16)
project(MTUTool VERSION 2.0.0 LANGUAGES CXX)

# ── C++ 标准 ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── MSVC 优化选项 ─────────────────────────────────────
if(MSVC)
    # 静态链接 CRT (/MT)，去除运行时依赖
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_compile_options(
        /O2          # 体积+速度优化
        /GS-         # 禁用缓冲区安全检查（减小体积）
        /Gy          # 函数级链接
        /EHs-c-      # 禁用 C++ 异常（减小体积）
        /GR-         # 禁用 RTTI（减小体积）
        /wd4100      # 忽略未引用形参警告
    )

    add_link_options(
        /OPT:REF     # 移除未引用代码
        /OPT:ICF     # 合并相同函数
    )
endif()

# ── 源文件 ────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/ui.cpp
    src/network.cpp
    src/utils.cpp
)

set(RESOURCES
    res/app.rc
)

add_executable(mtu-tool WIN32 ${SOURCES} ${RESOURCES})

# ── 链接库 ────────────────────────────────────────────
target_link_libraries(mtu-tool PRIVATE
    iphlpapi    # GetAdaptersAddresses / SetIpInterfaceEntry / Icmp*
    ws2_32      # inet_addr / getaddrinfo / WSAStartup
    advapi32    # CheckTokenMembership / 注册表 API
    comctl32    # ListView / 通用控件
    shell32     # ShellExecuteExW
)

# ── 资源包含路径 ──────────────────────────────────────
target_include_directories(mtu-tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)

# ── 生成 ico 图标（编译前自动执行）─────────────────────
set(ICO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
if(NOT EXISTS "${ICO_FILE}")
    add_custom_target(
        generate_icon ALL
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_icon.ps1"
            -OutFile "${ICO_FILE}"
        COMMENT "Generating app.ico..."
        VERBATIM
    )
    add_dependencies(mtu-tool generate_icon)
endif()

# ── UPX 压缩（可选，需要安装 UPX）────────────────────
# 编译后执行: upx --best --lzma mtu-tool.exe
