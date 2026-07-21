@echo off
setlocal EnableDelayedExpansion
echo === iCloud Mail Build ===

set GPP=
set BASH=

:: Check MSYS2 locations for g++
for %%P in (
    "C:\msys64\ucrt64\bin\g++.exe"
    "C:\msys64\mingw64\bin\g++.exe"
) do if exist %%P (
    set "GPP=%%~P"
    goto :found_gpp
)

:: Check PATH
where g++ >nul 2>&1 && (set "GPP=g++" & goto :found_gpp)

:: MSVC fallback
where cl >nul 2>&1 && goto :build_msvc
for %%R in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
) do if exist %%R (call %%R >nul 2>&1 & goto :build_msvc)

echo No compiler found.
echo Run: winget install MSYS2.MSYS2
echo Then: C:\msys64\usr\bin\bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc"
goto :end

:found_gpp
echo Compiler: !GPP!

:: Use MSYS2 bash if available (properly handles stderr)
if exist "C:\msys64\usr\bin\bash.exe" (
    set BASH=C:\msys64\usr\bin\bash.exe
    set "GPPPATH="
    if "!GPP!"=="C:\msys64\ucrt64\bin\g++.exe"  set "GPPPATH=/ucrt64/bin"
    if "!GPP!"=="C:\msys64\mingw64\bin\g++.exe" set "GPPPATH=/mingw64/bin"
    if defined GPPPATH (
        "!BASH!" -lc "export PATH=!GPPPATH!:$PATH && cd /c/tmp && g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows icloud_mail.cpp -o icloud_mail.exe -lws2_32 -lsecur32 -lcomctl32 -lshell32 -lole32 -loleaut32 -luuid"
    ) else (
        "!BASH!" -lc "cd /c/tmp && g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows icloud_mail.cpp -o icloud_mail.exe -lws2_32 -lsecur32 -lcomctl32 -lshell32 -lole32 -loleaut32 -luuid"
    )
) else (
    "!GPP!" -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows icloud_mail.cpp -o icloud_mail.exe -lws2_32 -lsecur32 -lcomctl32 -lshell32
)
goto :check

:build_msvc
echo Compiler: cl.exe
cl /O2 /EHsc /MT /DUNICODE /D_UNICODE /W3 /nologo icloud_mail.cpp /Fe:icloud_mail.exe /link /SUBSYSTEM:WINDOWS

:check
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
