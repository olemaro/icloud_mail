@echo off
setlocal
echo === iCloud Mail Build ===
cd /d "%~dp0"

set BASH=C:\msys64\usr\bin\bash.exe
if not exist "%BASH%" (
    echo MSYS2 not found at C:\msys64
    goto :end
)

:: Write build script — use full paths so windres/g++ are always found
set SCRIPT=C:\msys64\tmp\icloud_build.sh
(
echo #!/bin/bash
echo set -e
echo export PATH=/ucrt64/bin:/mingw64/bin:/usr/bin:$PATH
echo cd /c/tmp/icloud_mail
echo echo "[1/2] Compiling resources..."
echo windres icloud_mail.rc -O coff -o icloud_mail.res
echo echo "[2/2] Building exe..."
echo g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows \
echo     icloud_mail.cpp icloud_mail.res -o icloud_mail.exe \
echo     -lws2_32 -lsecur32 -lcomctl32 -lshell32 -lole32 -loleaut32 -luuid -lwinhttp
echo echo "Done."
) > "%SCRIPT%"

"%BASH%" -l "%SCRIPT%"

if exist icloud_mail.exe (
    echo.
    echo BUILD OK -- icloud_mail.exe
    if "%1"=="run" start icloud_mail.exe
) else (
    echo BUILD FAILED
)

:end
endlocal
pause
