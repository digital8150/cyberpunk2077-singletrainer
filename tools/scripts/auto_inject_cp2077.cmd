@echo off
setlocal
python "%~dp0inject.py" --auto %*
endlocal
