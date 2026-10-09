@echo off
rem Build AudioPlayer (C++/Win32 + miniaudio) with MinGW-w64 g++.
rem
rem Usage:
rem     build.bat
rem
rem If MinGW is not in C:\Users\1\Tools\mingw64, set MINGW_HOME first:
rem     set MINGW_HOME=C:\mingw64
setlocal
set "ROOT=%~dp0"
if "%MINGW_HOME%"=="" set "MINGW_HOME=C:\Users\1\Tools\mingw64"
set "GXX=%MINGW_HOME%\bin\g++.exe"
set "WINDRES=%MINGW_HOME%\bin\windres.exe"

rem По этому пути инструментов может не быть — например, в CI или при установке
rem MinGW в другое место. Тогда берём то, что уже лежит в PATH.
if not exist "%GXX%" (
    for %%I in (g++.exe) do if not "%%~$PATH:I"=="" set "GXX=%%~$PATH:I"
)
if not exist "%WINDRES%" (
    for %%I in (windres.exe) do if not "%%~$PATH:I"=="" set "WINDRES=%%~$PATH:I"
)

if not exist "%GXX%" (
    echo [error] g++ not found: %GXX%
    echo         Set MINGW_HOME to your MinGW-w64 folder and run again,
    echo         or add its bin folder to PATH.
    exit /b 1
)
if not exist "%WINDRES%" (
    echo [error] windres not found: %WINDRES%
    echo         windres ships with MinGW-w64 binutils.
    exit /b 1
)

pushd "%ROOT%"
if not exist build mkdir build
if not exist dist mkdir dist

set "CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -Isrc -Ivendor"

echo [1/4] resources (icon, version info)
"%WINDRES%" -I src -i resources.rc -o build\resources.o
if errorlevel 1 goto :fail

echo [2/4] player objects
for %%S in (common media_probe player_core playlist tags settings miniaudio_impl ui_slider app_win32 main) do (
    echo       %%S.cpp
    "%GXX%" %CXXFLAGS% -c src\%%S.cpp -o build\%%S.o
    if errorlevel 1 goto :fail
)

echo [3/4] linking dist\AudioPlayer.exe
"%GXX%" %CXXFLAGS% -mwindows -static -s -o dist\AudioPlayer.exe ^
    build\common.o build\media_probe.o build\player_core.o build\playlist.o build\tags.o ^
    build\settings.o build\miniaudio_impl.o build\ui_slider.o build\app_win32.o build\main.o ^
    build\resources.o ^
    -lole32 -lwinmm -luuid -luser32 -lgdi32 -lcomdlg32 -lshell32 -lshlwapi
if errorlevel 1 goto :fail

echo [4/4] linking test binaries
"%GXX%" %CXXFLAGS% -c tests\test_core.cpp -o build\test_core.o
if errorlevel 1 goto :fail
"%GXX%" %CXXFLAGS% -static -s -o dist\test_core.exe ^
    build\test_core.o build\common.o build\media_probe.o build\player_core.o build\playlist.o ^
    build\tags.o build\settings.o build\miniaudio_impl.o ^
    -lole32 -lwinmm -luuid
if errorlevel 1 goto :fail

"%GXX%" %CXXFLAGS% -c tests\test_playlist.cpp -o build\test_playlist.o
if errorlevel 1 goto :fail
"%GXX%" %CXXFLAGS% -static -s -o dist\test_playlist.exe build\test_playlist.o build\playlist.o ^
    build\common.o
if errorlevel 1 goto :fail

"%GXX%" %CXXFLAGS% -c tests\test_tags.cpp -o build\test_tags.o
if errorlevel 1 goto :fail
"%GXX%" %CXXFLAGS% -static -s -o dist\test_tags.exe build\test_tags.o build\tags.o build\common.o ^
    -lole32 -luser32
if errorlevel 1 goto :fail

"%GXX%" %CXXFLAGS% -c tests\test_settings.cpp -o build\test_settings.o
if errorlevel 1 goto :fail
"%GXX%" %CXXFLAGS% -static -s -o dist\test_settings.exe build\test_settings.o build\settings.o ^
    build\common.o
if errorlevel 1 goto :fail

echo.
echo Done:
echo     dist\AudioPlayer.exe    - the player
echo     dist\test_core.exe      - core self-test (run from the project root)
echo     dist\test_playlist.exe  - shuffle/repeat/playlist self-test
echo     dist\test_tags.exe      - tag reader self-test
echo     dist\test_settings.exe  - settings save/load self-test
popd
exit /b 0

:fail
echo.
echo [error] build failed
popd
exit /b 1
