# 运行 HUD 实时演示(默认约 25 秒, 适合录屏)
#
# 用法:
#   .\scripts\run_demo.ps1                    # 默认: 英→中, 实时节奏
#   .\scripts\run_demo.ps1 -Scenario zh2en    # 中→英
#   .\scripts\run_demo.ps1 -Scenario bench -Fast
param(
    [ValidateSet('en2zh', 'zh2en', 'bench')]
    [string]$Scenario = 'en2zh',
    [switch]$Fast,
    [switch]$NoDraft,
    [switch]$NoColor
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root/build/sublens_demo.exe"
if (-not (Test-Path $exe)) {
    Write-Host "没找到 sublens_demo.exe, 先编译..." -ForegroundColor Yellow
    & "$PSScriptRoot/build.ps1"
}
$exe = "$root/build/sublens_demo.exe"

# 中文控制台 + ANSI 支持(Windows Terminal / Windows 10+ 控制台均可)
chcp 65001 > $null
$demoArgs = @($Scenario)
if ($Fast) { $demoArgs += '--fast' }
if ($NoDraft) { $demoArgs += '--no-draft' }
if ($NoColor) { $demoArgs += '--no-color' }

& $exe @demoArgs
