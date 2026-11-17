@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================
rem  Benchmarks (~1 minute, prints Markdown tables)
rem    bench.cmd                run all
rem    bench.cmd latency        end-to-end latency only
rem    bench.cmd ring vad       selected items
rem  ASCII-only text: see build.cmd header for why.
rem ===========================================================

if not exist "build\sublens_bench.exe" (
  echo Not built yet, building first...
  call build.cmd
  if errorlevel 1 exit /b 1
)

if not exist "out" mkdir out
build\sublens_bench.exe %*
echo.
echo Tip: save a full run with  bench.cmd ^> out\bench.txt
exit /b %errorlevel%
