@echo off
REM Builds hwwatch32.dll using E:\msys64\mingw32
REM Requires JAVA_HOME to point at a JDK (needs include\jni.h and include\win32\jni_md.h)

if "%JAVA_HOME%"=="" (
    echo ERROR: JAVA_HOME is not set.
    echo   set JAVA_HOME=C:\Program Files\Java\jdk1.8.0_XXX
    echo ^(no trailing backslash, no quotes^)
    pause
    exit /b 1
)

echo Using JAVA_HOME=%JAVA_HOME%

if not exist "%JAVA_HOME%\include\jni.h" (
    echo ERROR: "%JAVA_HOME%\include\jni.h" not found.
    echo This usually means JAVA_HOME points at a JRE instead of a JDK -
    echo JRE installs do not ship jni.h. Point JAVA_HOME at a full JDK.
    pause
    exit /b 1
)

if not exist "%JAVA_HOME%\include\win32\jni_md.h" (
    echo ERROR: "%JAVA_HOME%\include\win32\jni_md.h" not found.
    echo Your JDK include folder is missing the win32 subfolder - unusual,
    echo double check this is a genuine Windows JDK install.
    pause
    exit /b 1
)

set PATH=E:\msys64\mingw32\bin;%PATH%

where gcc >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: gcc not found on PATH after adding E:\msys64\mingw32\bin
    echo Check that E:\msys64\mingw32\bin\gcc.exe actually exists.
    pause
    exit /b 1
)

gcc -m32 -shared -O2 -Wall ^
    -I"%JAVA_HOME%\include" -I"%JAVA_HOME%\include\win32" ^
    -o hwwatch32.dll hwwatcher.c ^
    -lsetupapi -lcfgmgr32 -luser32 -ladvapi32 -static-libgcc

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    pause
    exit /b 1
)
echo Built hwwatch32.dll
pause
