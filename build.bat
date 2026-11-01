@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

echo ============================================================
echo   StarCraft: Ghost (Windows 11 Port) Build Script
echo ============================================================
echo.

REM 1. Locate CMake (prefer PATH, otherwise auto-detect Visual Studio CMake)
set "CMAKE_EXE="
where cmake >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set "CMAKE_EXE=cmake"
) else (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
            if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
                set "CMAKE_EXE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            )
        )
    )
    if "!CMAKE_EXE!"=="" (
        for /f "delims=" %%F in ('dir /b /s "%ProgramFiles%\Microsoft Visual Studio\cmake.exe" 2^>nul') do (
            set "CMAKE_EXE=%%F"
        )
    )
)

if "!CMAKE_EXE!"=="" (
    echo [ERROR] CMake was not found. Please install Visual Studio C++ tools or CMake.
    echo.
    pause
    exit /b 1
)

echo [INFO] Using CMake: "!CMAKE_EXE!"
echo.

REM 2. Configure 32-bit (Win32) project if build/CMakeCache.txt does not exist
if not exist "build\CMakeCache.txt" (
    echo [STEP 1/2] Configuring 32-bit ^(Win32^) CMake project...
    "!CMAKE_EXE!" -B build -A Win32
    if !ERRORLEVEL! NEQ 0 (
        echo.
        echo [ERROR] CMake configuration failed!
        pause
        exit /b !ERRORLEVEL!
    )
    echo.
)

REM 3. Build Release target (pass "clean" argument to force full rebuild: build.bat clean)
set "CLEAN_FLAG="
if /i "%~1"=="clean" (
    set "CLEAN_FLAG=--clean-first"
    echo [STEP 2/2] Performing clean rebuild ^(Release^)...
) else (
    echo [STEP 2/2] Building GhostWin11.exe ^(Release^)...
)

"!CMAKE_EXE!" --build build --config Release !CLEAN_FLAG!
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo ============================================================
    echo   [FAILED] Build failed! Please check the error output above.
    echo ============================================================
    pause
    exit /b !ERRORLEVEL!
)

echo.
echo ============================================================
echo   [SUCCESS] Build completed successfully!
echo   Output: %~dp0build\bin\GhostWin11.exe
echo ============================================================
echo.

echo %CMDCMDLINE% | find /i "%~0" >nul
if not errorlevel 1 pause
exit /b 0
