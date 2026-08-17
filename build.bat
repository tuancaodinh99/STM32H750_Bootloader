@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM STM32H750 Bootloader Build Script (Command Line)
REM Usage: build.bat          - interactive build (pauses at end)
REM        build.bat --no-pause - build without pausing (for CI/automation)
REM ============================================================

set "NO_PAUSE="
if /I "%~1"=="--no-pause" set "NO_PAUSE=1"


echo.
echo ======================================
echo STM32H750 Bootloader Build
echo ======================================
echo.

cd /d "%~dp0"

REM ============================================================
REM STM32CubeIDE toolchain paths (auto-detected)
REM ============================================================
set "IDE_ROOT=C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins"

set "GCC_DIR=%IDE_ROOT%\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools\bin"
set "MAKE_DIR=%IDE_ROOT%\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845\tools\bin"

set "PATH=%GCC_DIR%;%MAKE_DIR%;%PATH%"

REM Check if make is available
where make >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: make command not found
    echo Looked in: %MAKE_DIR%
    pause
    exit /b 1
)
echo [INFO] Found make command

REM Check if arm-none-eabi-gcc is available
where arm-none-eabi-gcc >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: arm-none-eabi-gcc not found
    echo Looked in: %GCC_DIR%
    pause
    exit /b 1
)
echo [INFO] Found ARM GNU tools
echo.

REM ============================================================
REM STEP 1: Clean previous build artifacts (in Debug folder)
REM ============================================================
echo [STEP 1/4] Cleaning previous build...
cd /d "%~dp0Debug"
make clean 2>nul
echo [OK] Clean done
echo.

REM ============================================================
REM STEP 2: Build
REM ============================================================
echo [STEP 2/4] Building project...
echo.
make all -j16
if !ERRORLEVEL! EQU 0 (
    echo.
    echo [OK] Build completed successfully!
) else (
    echo.
    echo [ERROR] Build failed! Check errors above.
    pause
    exit /b 1
)
echo.

REM ============================================================
REM STEP 3: Verify output
REM ============================================================
echo [STEP 3/4] Verifying output files...
cd /d "%~dp0"
if exist "Debug\STM32H750_Bootloader.elf" (
    echo [OK] Found STM32H750_Bootloader.elf
    for %%A in ("Debug\STM32H750_Bootloader.elf") do (
        set size=%%~zA
        echo [INFO] File size: !size! bytes
    )
) else (
    echo [ERROR] Output file not found: Debug\STM32H750_Bootloader.elf
    pause
    exit /b 1
)
echo.

REM ============================================================
REM STEP 4: Summary
REM ============================================================
echo [STEP 4/4] Build Summary
echo ======================================
echo Project:        STM32H750_Bootloader
echo Configuration:  Debug
echo Output:         Debug\STM32H750_Bootloader.elf
echo Toolchain:      GNU Tools for STM32 (13.3.rel1)
echo Status:         SUCCESS
echo ======================================
echo.
echo Next steps:
echo   1. Flash using STM32CubeProgrammer
echo   2. Or use: openocd (if configured)
echo.
if not defined NO_PAUSE pause

