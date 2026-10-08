@echo off
rem Developer build of the runtime with the Visual Studio Build Tools (CMake + Ninja) into build\native.
rem Needs C sources generated first: python -m ircrecomp lift game\RAL.EXE build\gen
set "ROOT=%~dp0.."
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%ROOT%" -B "%ROOT%\build\native" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%ROOT%\build\native" --config Release %* || exit /b 1
