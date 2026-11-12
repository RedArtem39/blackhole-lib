@echo off
setlocal
cd /d "%~dp0"

if not defined VULKAN_SDK (
    echo VULKAN_SDK is not set
    exit /b 1
)

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1 || exit /b 1

if not exist build\shaders mkdir build\shaders

set GLSLC="%VULKAN_SDK%\Bin\glslc.exe"
%GLSLC% -O src\shaders\fullscreen.vert -o build\shaders\fullscreen.vert.spv || exit /b 1
%GLSLC% -O src\shaders\scene.frag      -o build\shaders\scene.frag.spv      || exit /b 1
%GLSLC% -O src\shaders\composite.frag  -o build\shaders\composite.frag.spv  || exit /b 1

cl /nologo /std:c++20 /O2 /EHsc /W3 /MT /utf-8 src\main.cpp /I"%VULKAN_SDK%\Include" ^
   /Fo:build\ /Fe:build\BlackHole.exe ^
   /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib user32.lib gdi32.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup || exit /b 1

echo OK: build\BlackHole.exe
