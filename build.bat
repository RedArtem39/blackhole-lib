@echo off
rem blackhole-lib build: static lib, DLL, and examples.  Run by full path (a different build.bat is on PATH).
setlocal
cd /d "%~dp0"

if not defined VULKAN_SDK (
    echo VULKAN_SDK is not set
    exit /b 1
)
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1 || exit /b 1

if not exist build\gen mkdir build\gen
if not exist build\obj\static mkdir build\obj\static
if not exist build\obj\dll mkdir build\obj\dll
if not exist build\obj\ex mkdir build\obj\ex

rem ---- shaders -> SPIR-V embedded as C arrays
set GLSLC="%VULKAN_SDK%\Bin\glslc.exe"
%GLSLC% -O -mfmt=num -Ishaders shaders\fullscreen.vert -o build\gen\fullscreen.vert.inc || exit /b 1
%GLSLC% -O -mfmt=num -Ishaders shaders\scene.frag      -o build\gen\scene.frag.inc      || exit /b 1
%GLSLC% -O -mfmt=num -Ishaders shaders\composite.frag  -o build\gen\composite.frag.inc  || exit /b 1

set CXXFLAGS=/nologo /std:c++20 /O2 /EHsc /W3 /MT /utf-8 /Iinclude /Ibuild\gen /I"%VULKAN_SDK%\Include"
set LIBS=/LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib user32.lib gdi32.lib

rem ---- static library
cl %CXXFLAGS% /c src\blackhole.cpp /Fo:build\obj\static\blackhole.obj || exit /b 1
lib /nologo /OUT:build\blackhole_static.lib build\obj\static\blackhole.obj || exit /b 1

rem ---- DLL (+ import lib blackhole.lib)
cl %CXXFLAGS% /DBH_BUILD_DLL /c src\blackhole.cpp /Fo:build\obj\dll\blackhole.obj || exit /b 1
link /nologo /DLL /OUT:build\blackhole.dll /IMPLIB:build\blackhole.lib build\obj\dll\blackhole.obj %LIBS% || exit /b 1

rem ---- examples
cl %CXXFLAGS% examples\viewer.cpp /Fo:build\obj\ex\ /Fe:build\viewer.exe ^
   /link build\blackhole_static.lib %LIBS% /SUBSYSTEM:CONSOLE || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude examples\wallpaper.c /Fo:build\obj\ex\ /Fe:build\wallpaper.exe ^
   /link build\blackhole_static.lib %LIBS% || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude /DBH_USE_DLL examples\gallery.c /Fo:build\obj\ex\ /Fe:build\gallery.exe ^
   /link build\blackhole.lib || exit /b 1

echo OK
