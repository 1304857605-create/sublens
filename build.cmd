@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================
rem  SubLens build script (ASCII only on purpose: cmd.exe parses
rem  .bat/.cmd in the OEM code page, so Chinese text here would
rem  be garbled and executed as commands on non-UTF8 consoles.)
rem  Usage: double-click, or run "build.cmd" in a terminal.
rem ===========================================================

echo == configure (Ninja, Release) ==
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail

echo == build ==
cmake --build build
if errorlevel 1 goto :fail

echo.
echo Done. Artifacts:
echo   build\sublens_demo.exe    realtime HUD demo
echo   build\sublens_tests.exe   unit tests
echo   build\sublens_bench.exe   benchmarks
echo.
echo Next: run  demo.cmd   to watch the live subtitles
echo       run  test.cmd   to run the 67 unit tests
exit /b 0

:fail
echo.
echo BUILD FAILED - please send the error above.
exit /b 1
