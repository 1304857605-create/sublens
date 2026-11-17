@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ===========================================================
rem  Live HUD demo (~24 seconds)
rem    demo.cmd                    EN speech -^> ZH subtitles (default)
rem    demo.cmd zh2en              ZH speech -^> EN subtitles
rem    demo.cmd en2zh --fast       fast mode (~9s, no latency stats)
rem    demo.cmd en2zh --no-draft   disable the draft line
rem  ASCII-only text: see build.cmd header for why.
rem ===========================================================

if not exist "build\sublens_demo.exe" (
  echo Not built yet, building first...
  call build.cmd
  if errorlevel 1 exit /b 1
)

if "%~1"=="" (
  build\sublens_demo.exe en2zh
) else (
  build\sublens_demo.exe %*
)

exit /b %errorlevel%
