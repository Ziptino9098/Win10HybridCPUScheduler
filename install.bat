@echo off
echo ============================================
echo   HybridCPUWin10 - Installer
echo   Windows 10 Hybrid CPU Scheduler Driver
echo ============================================
echo.

REM --- Check for Administrator privileges ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] This installer must be run as Administrator.
    pause
    exit /b 1
)

echo [1/5] Enabling Test Signing Mode...
bcdedit /set testsigning on
if %errorlevel% neq 0 (
    echo [ERROR] Failed to enable test signing.
    pause
    exit /b 1
)

echo [2/5] Copying driver to System32\drivers...
copy /y HybridCPU.sys C:\Windows\System32\drivers\ >nul
if %errorlevel% neq 0 (
    echo [ERROR] Failed to copy HybridCPU.sys.
    pause
    exit /b 1
)

echo [3/5] Installing test certificate...
certutil -addstore "TrustedPublisher" HybridCPU.cer >nul
certutil -addstore "Root" HybridCPU.cer >nul

echo [4/5] Creating service entry (SYSTEM_START)...
sc create HybridCPU type= kernel start= system binPath= "C:\Windows\System32\drivers\HybridCPU.sys" >nul 2>&1

if %errorlevel% neq 0 (
    echo [INFO] Service already exists, continuing...
)

echo [5/5] Starting driver service...
sc start HybridCPU >nul 2>&1

if %errorlevel% neq 0 (
    echo [WARNING] Driver did not start automatically.
    echo A reboot may be required first.
) else (
    echo [SUCCESS] HybridCPU driver is now running!
)

echo.
echo ============================================
echo   Installation complete.
echo   Please reboot to activate Test Signing.
echo ============================================
pause
