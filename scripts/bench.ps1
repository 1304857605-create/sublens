# 跑基准测试并保存原始 Markdown 输出
#
# 用法:
#   .\scripts\bench.ps1              # 全部基准(约 1 分钟, 含实时节奏场景)
#   .\scripts\bench.ps1 ring vad     # 只跑指定项
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Only
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root/build/sublens_bench.exe"
if (-not (Test-Path $exe)) {
    & "$PSScriptRoot/build.ps1"
}

if (-not (Test-Path "$root/out")) { New-Item -ItemType Directory -Path "$root/out" | Out-Null }
chcp 65001 > $null

if ($Only) {
    & $exe @Only | Tee-Object -FilePath "$root/out/benchmark-raw.md"
} else {
    & $exe | Tee-Object -FilePath "$root/out/benchmark-raw.md"
}
Write-Host "`n原始输出已保存: out/benchmark-raw.md" -ForegroundColor Green
Write-Host "把其中的表格粘到 docs/BENCHMARK.md 即可更新文档。" -ForegroundColor DarkGray
