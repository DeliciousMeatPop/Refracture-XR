@echo off
rem Builds the Digitalis CPU backend (refract_digitalis.lib) into Refract\build-native-digitalis with clang-cl.
setlocal
set VSCMD_SKIP_SENDTELEMETRY=1
if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined ANDROID_SDK set "ANDROID_SDK=%USERPROFILE%\Android\Sdk"
if not defined REFRACT_LLVM set "REFRACT_LLVM=%~dp0..\..\..\external\llvm"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ANDROID_SDK%\cmake\3.31.6\bin;%PATH%"
set "SRC=%~dp0."
set "OUT=%~dp0..\..\build-native-digitalis"
if not exist "%OUT%\build.ninja" (
  cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    "-DCMAKE_C_COMPILER=%REFRACT_LLVM%\bin\clang-cl.exe" "-DCMAKE_CXX_COMPILER=%REFRACT_LLVM%\bin\clang-cl.exe" ^
    "-DCMAKE_AR=%REFRACT_LLVM%\bin\llvm-lib.exe" || exit /b 1
)
cmake --build "%OUT%" %*
