@echo off
:: Windows Error Reporting (WER) LocalDumps 등록 배치 파일 (관리자 권한 실행 필요)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0enable_localdumps.ps1"
pause
