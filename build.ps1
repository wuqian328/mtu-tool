# ── build.ps1 ───────────────────────────────────────
#   使用 MSVC BuildTools 直接编译 mtu-tool
#   无需 CMake，直接调用 cl.exe / rc.exe / link.exe
# ─────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
Push-Location $projectRoot

try {
    # ── 路径配置 ────────────────────────────────────
    $vsRoot  = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    $msvcVer = "14.44.35207"
    $sdkVer  = "10.0.26100.0"
    $sdkRoot = "C:\Program Files (x86)\Windows Kits\10"

    $msvcBin   = "$vsRoot\VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64"
    $msvcInc   = "$vsRoot\VC\Tools\MSVC\$msvcVer\include"
    $msvcLib   = "$vsRoot\VC\Tools\MSVC\$msvcVer\lib\x64"
    $sdkBin    = "$sdkRoot\bin\$sdkVer\x64"
    $sdkInc    = "$sdkRoot\Include\$sdkVer"
    $sdkLib    = "$sdkRoot\Lib\$sdkVer"

    $cl   = "$msvcBin\cl.exe"
    $rc   = "$sdkBin\rc.exe"
    $link = "$msvcBin\link.exe"

    # 验证工具
    foreach ($tool in @($cl, $rc, $link)) {
        if (-not (Test-Path $tool)) { Write-Error "找不到: $tool"; exit 1 }
    }
    Write-Host "cl.exe : $cl" -ForegroundColor Green
    Write-Host "rc.exe : $rc" -ForegroundColor Green
    Write-Host "link   : $link" -ForegroundColor Green

    # ── 设置环境 ────────────────────────────────────
    $env:PATH = "$msvcBin;$sdkBin;$env:PATH"
    $includes = @(
        $msvcInc,
        "$sdkInc\ucrt",
        "$sdkInc\um",
        "$sdkInc\shared",
        "$sdkInc\winrt",
        "$sdkInc\cppwinrt"
    ) -join ";"
    $env:INCLUDE = $includes

    $libs = @(
        $msvcLib,
        "$sdkLib\ucrt\x64",
        "$sdkLib\um\x64"
    ) -join ";"
    $env:LIB = $libs

    # ── 确保图标 ────────────────────────────────────
    $icoFile = "$projectRoot\res\app.ico"
    if (-not (Test-Path $icoFile)) {
        Write-Host "Generating icon..." -ForegroundColor Yellow
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$projectRoot\scripts\gen_icon.ps1" -OutFile $icoFile
    }

    # 创建 build 目录
    $buildDir = "$projectRoot\build"
    if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

    # ── [1/3] 编译资源 ──────────────────────────────
    Write-Host "`n[1/3] Compiling resources (rc.exe)..." -ForegroundColor Cyan
    $resObj = "$buildDir\app.res"
    $rcArgs = @(
        "/nologo",
        "/fo", $resObj,
        "$projectRoot\res\app.rc"
    )
    & $rc $rcArgs
    if ($LASTEXITCODE -ne 0) { throw "rc.exe failed ($LASTEXITCODE)" }
    Write-Host "  OK: app.res" -ForegroundColor Green

    # ── [2/3] 编译 C++ ──────────────────────────────
    Write-Host "`n[2/3] Compiling C++ (cl.exe)..." -ForegroundColor Cyan

    $clCommon = @(
        "/nologo", "/c",
        "/O2", "/GS-", "/Gy",        # 优化
        "/EHs-c-", "/GR-",           # 禁用异常/RTTI
        "/MT",                        # 静态 CRT
        "/utf-8",                    # 源文件 UTF-8 编码
        "/D", "NTDDI_VERSION=0x06010000",
        "/D", "_WIN32_WINNT=0x0601",
        "/D", "_CRT_SECURE_NO_WARNINGS",
        "/D", "_WINSOCK_DEPRECATED_NO_WARNINGS",
        "/W3", "/wd4100", "/wd4530",
        "/D", "UNICODE",
        "/D", "_UNICODE",
        "/D", "NDEBUG",
        "/I", "$projectRoot\src",
        "/I", "$projectRoot\res"
    )

    $srcFiles = Get-ChildItem "$projectRoot\src\*.cpp"
    $objFiles = @()
    foreach ($src in $srcFiles) {
        $obj = "$buildDir\$($src.BaseName).obj"
        Write-Host "  $($src.Name) -> $($src.BaseName).obj"

        & $cl ($clCommon + "/Fo`"$obj`"" + "`"$($src.FullName)`"")
        if ($LASTEXITCODE -ne 0) { throw "cl.exe failed for $($src.Name) ($LASTEXITCODE)" }
        $objFiles += $obj
    }
    Write-Host "  $($objFiles.Count) object files compiled" -ForegroundColor Green

    # ── [3/3] 链接 ──────────────────────────────────
    Write-Host "`n[3/3] Linking (link.exe)..." -ForegroundColor Cyan

    $exeOutput = "$projectRoot\mtu-tool.exe"
    $allObjs = ($objFiles + $resObj) -join " "

    $linkLibs = @(
        "user32.lib",
        "gdi32.lib",
        "iphlpapi.lib",
        "ws2_32.lib",
        "advapi32.lib",
        "comctl32.lib",
        "shell32.lib",
        "kernel32.lib"
    ) -join " "

    $linkArgs = @(
        "/NOLOGO",
        "/SUBSYSTEM:WINDOWS",
        "/OPT:REF",
        "/OPT:ICF",
        "/OUT:$exeOutput"
    )

    Write-Host "  Linking..."
    $allArgs = $linkArgs + $objFiles + $resObj + @("user32.lib","gdi32.lib","iphlpapi.lib","ws2_32.lib","advapi32.lib","comctl32.lib","shell32.lib","kernel32.lib")
    & $link $allArgs
    if ($LASTEXITCODE -ne 0) { throw "link.exe failed ($LASTEXITCODE)" }

    # ── 结果 ────────────────────────────────────────
    if (Test-Path $exeOutput) {
        $sizeKB = [math]::Round((Get-Item $exeOutput).Length / 1024, 1)
        Write-Host "`n========================================" -ForegroundColor Green
        Write-Host "  BUILD SUCCESSFUL!" -ForegroundColor Green
        Write-Host "  Output : mtu-tool.exe" -ForegroundColor Green
        Write-Host "  Size   : $sizeKB KB" -ForegroundColor Green
        Write-Host "========================================" -ForegroundColor Green
    } else {
        throw "Output file not found"
    }
} finally {
    Pop-Location
}
