@echo off
rem One-click: recompile International Rally Championship from a CD image (.cue).
rem usage: ircrecomp.bat path\to\IRC.cue [output folder]
setlocal
set ROOT=%~dp0
if "%~1"=="" (
  echo usage: %~nx0 path\to\game.cue [output folder]
  exit /b 1
)
set OUT=%~2
if "%OUT%"=="" set OUT=%ROOT%dist\IRC
if not exist "%ROOT%.venv\Scripts\python.exe" (
  echo creating Python environment ...
  python -m venv "%ROOT%.venv" || exit /b 1
  "%ROOT%.venv\Scripts\python.exe" -m pip install -q -r "%ROOT%requirements.txt" || exit /b 1
)
pushd "%ROOT%"
"%ROOT%.venv\Scripts\python.exe" -m ircrecomp build "%~f1" --out "%OUT%"
set RC=%ERRORLEVEL%
popd
exit /b %RC%
