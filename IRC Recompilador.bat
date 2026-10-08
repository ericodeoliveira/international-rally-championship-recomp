@echo off
rem Abre a janela do recompilador do International Rally Championship.
setlocal
set ROOT=%~dp0
if not exist "%ROOT%.venv\Scripts\pythonw.exe" (
  echo Preparando o ambiente Python (so na primeira vez^)...
  python -m venv "%ROOT%.venv" || goto nopython
  "%ROOT%.venv\Scripts\python.exe" -m pip install -q -r "%ROOT%requirements.txt" || goto nopython
)
cd /d "%ROOT%"
start "" "%ROOT%.venv\Scripts\pythonw.exe" -m ircrecomp.gui
exit /b 0
:nopython
echo.
echo Nao foi possivel preparar o Python. Instale o Python 3 em https://www.python.org/downloads/
echo (marque "Add python.exe to PATH") e abra este arquivo de novo.
pause
exit /b 1
