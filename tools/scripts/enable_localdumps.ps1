# Windows Error Reporting (WER) LocalDumps 등록 스크립트
# 관리자 권한 PowerShell에서 실행하면 Cyberpunk 2077 크래시 발생 시 풀 미니덤프(.dmp)를 %LOCALAPPDATA%\CrashDumps 에 자동 생성합니다.

$dumpDir = "$env:LOCALAPPDATA\CrashDumps"
if (!(Test-Path -Path $dumpDir)) {
    New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
}

$keyPath = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Cyberpunk2077.exe"
New-Item -Path $keyPath -Force | Out-Null
Set-ItemProperty -Path $keyPath -Name "DumpFolder" -Value $dumpDir -Type ExpandString
Set-ItemProperty -Path $keyPath -Name "DumpCount" -Value 10 -Type DWord
Set-ItemProperty -Path $keyPath -Name "DumpType" -Value 2 -Type DWord

Write-Host "WER LocalDumps successfully configured for Cyberpunk2077.exe!" -ForegroundColor Green
Write-Host "Dump folder: $dumpDir (Type=Full, Count=10)" -ForegroundColor Cyan
