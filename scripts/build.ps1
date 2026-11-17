# SubLens 构建与运行脚本
#
# 用法:
#   .\scripts\build.ps1            # 配置 + 编译
#   .\scripts\build.ps1 -Clean     # 清空 build 重新编译
#   .\scripts\build.ps1 -Test      # 编译后跑单元测试
param(
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    if ($Clean -and (Test-Path build)) {
        Remove-Item -Recurse -Force build
    }
    Write-Host "== 配置 (Ninja, Release) ==" -ForegroundColor Cyan
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败" }

    Write-Host "== 编译 ==" -ForegroundColor Cyan
    cmake --build build
    if ($LASTEXITCODE -ne 0) { throw "编译失败" }

    if ($Test) {
        Write-Host "== 单元测试 ==" -ForegroundColor Cyan
        & "$root/build/sublens_tests.exe"
        if ($LASTEXITCODE -ne 0) { throw "测试未通过" }
    }
    Write-Host "完成。产物: build/sublens_demo.exe, build/sublens_tests.exe, build/sublens_bench.exe" -ForegroundColor Green
}
finally {
    Pop-Location
}
