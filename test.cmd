@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================
rem  Unit tests (67 cases, no network / no GPU needed)
rem    test.cmd              run all
rem    test.cmd Scheduler    only cases whose name contains "Scheduler"
rem  ASCII-only text: see build.cmd header for why.
rem ===========================================================

if not exist "build\sublens_tests.exe" (
  echo Not built yet, building first...
  call build.cmd
  if errorlevel 1 exit /b 1
)

build\sublens_tests.exe %*
exit /b %errorlevel%
