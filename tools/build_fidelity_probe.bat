@echo off
setlocal
cd /d "%~dp0\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set DEP=%CD%\build\_deps\llama_cpp-build
set SRCI=%CD%\build\_deps\llama_cpp-src\include
set SRCGI=%CD%\build\_deps\llama_cpp-src\ggml\include
set CUDA_LIB=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\lib\x64
set VK_LIB=C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib

cl /nologo /EHsc /O2 /Ob2 /DNDEBUG /MT /utf-8 /std:c++20 ^
   /I "%SRCI%" /I "%SRCGI%" ^
   tools\fidelity_probe.cpp ^
   /Fe:tools\fidelity_probe.exe /Fo:tools\fidelity_probe.obj ^
   /link /machine:x64 /INCREMENTAL:NO /subsystem:console ^
   /DELAYLOAD:cublas64_13.dll /DELAYLOAD:cublasLt64_13.dll /DELAYLOAD:cudart64_13.dll /DELAYLOAD:vulkan-1.dll ^
   "%DEP%\src\llama.lib" ^
   "%DEP%\ggml\src\ggml.lib" ^
   "%DEP%\ggml\src\ggml-cpu.lib" ^
   "%DEP%\ggml\src\ggml-cuda\ggml-cuda.lib" ^
   "%CUDA_LIB%\cudart_static.lib" ^
   "%CUDA_LIB%\cublas.lib" ^
   "%CUDA_LIB%\cublasLt.lib" ^
   "%CUDA_LIB%\cuda.lib" ^
   "%DEP%\ggml\src\ggml-vulkan\ggml-vulkan.lib" ^
   "%DEP%\ggml\src\ggml-base.lib" ^
   "%VK_LIB%" ^
   delayimp.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib ^
   > tools\fidelity_probe_build.log 2>&1

set CL_RC=%ERRORLEVEL%
echo CL_RC=%CL_RC%
if exist tools\fidelity_probe.exe (
  echo BUILD_OK
) else (
  echo BUILD_FAIL
  type tools\fidelity_probe_build.log
)
