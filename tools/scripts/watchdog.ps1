<#
.SYNOPSIS
Silently watches a live game/trainer process and reports only abnormal conditions.

.DESCRIPTION
The watchdog produces no periodic status output. It writes one alert and exits when it detects:

- an appended [FATAL][unhandled] trainer record;
- process exit;
- consecutive Windows Responding=False samples; or
- a stale trainer log heartbeat.

Stop the watchdog before intentionally unloading the trainer, otherwise the stopped log heartbeat is expected to
look like a freeze. Use -DisableHeartbeat when trainer logging is intentionally disabled.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/scripts/watchdog.ps1 -TargetPid 30148

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/scripts/watchdog.ps1 -Name Cyberpunk2077.exe
#>
[CmdletBinding()]
param(
    [ValidateRange(0, [int]::MaxValue)]
    [int]$TargetPid = 0,

    [string]$Name = 'Cyberpunk2077.exe',

    [string]$LogPath = $(
        if ($env:CBPK_LOG_DIR) {
            Join-Path $env:CBPK_LOG_DIR 'cp2077_trainer.log'
        }
        else {
            Join-Path $env:LOCALAPPDATA 'cp2077_trainer\cp2077_trainer.log'
        }
    ),

    [string]$FatalLogPath = $(
        if ($env:CBPK_LOG_DIR) {
            Join-Path $env:CBPK_LOG_DIR 'cp2077_fatal.log'
        }
        else {
            Join-Path $env:LOCALAPPDATA 'cp2077_trainer\cp2077_fatal.log'
        }
    ),

    [ValidateRange(100, 60000)]
    [int]$PollMilliseconds = 2000,

    [ValidateRange(1, 100)]
    [int]$UnresponsivePolls = 3,

    [ValidateRange(1, 3600)]
    [int]$HeartbeatTimeoutSeconds = 20,

    [ValidateRange(1, 100)]
    [int]$StaleHeartbeatPolls = 3,

    [switch]$DisableHeartbeat,

    [switch]$DisableFatal,

    # A silent finite run for smoke tests and automation. Zero means run until an alert or Ctrl-C.
    [ValidateRange(0, [int]::MaxValue)]
    [int]$MaxPolls = 0
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$ExitProcessExited = 10
$ExitUnresponsive = 11
$ExitHeartbeatStale = 12
$ExitUnhandledFatal = 13
$ExitInvalidTarget = 2

function Resolve-TargetProcess {
    if ($TargetPid -gt 0) {
        return Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
    }

    $baseName = [IO.Path]::GetFileNameWithoutExtension($Name)
    $matches = @(Get-Process -Name $baseName -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 1) {
        return $matches[0]
    }
    if ($matches.Count -gt 1) {
        Write-Output "[WATCHDOG][ERROR] multiple processes named $Name; specify -TargetPid"
        exit $ExitInvalidTarget
    }
    return $null
}

function Read-AppendedText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [long]$Offset
    )

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek($Offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        try {
            return $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

$target = Resolve-TargetProcess
if ($null -eq $target) {
    $description = if ($TargetPid -gt 0) { "pid=$TargetPid" } else { "name=$Name" }
    Write-Output "[WATCHDOG][PROCESS_EXIT] $description is not running"
    exit $ExitProcessExited
}

$watchedPid = $target.Id
$lastLogLength = if (Test-Path -LiteralPath $LogPath) {
    (Get-Item -LiteralPath $LogPath).Length
}
else {
    0L
}
$lastHeartbeatUtc = [DateTime]::UtcNow
$fatalOffset = if (Test-Path -LiteralPath $FatalLogPath) {
    (Get-Item -LiteralPath $FatalLogPath).Length
}
else {
    0L
}
$unresponsiveCount = 0
$staleHeartbeatCount = 0
$pollCount = 0

while ($true) {
    if (-not $DisableFatal -and (Test-Path -LiteralPath $FatalLogPath)) {
        try {
            $fatalLength = (Get-Item -LiteralPath $FatalLogPath).Length
            if ($fatalLength -lt $fatalOffset) {
                # A rotated or truncated file starts a new baseline. Do not rescan historical records.
                $fatalOffset = $fatalLength
            }
            elseif ($fatalLength -gt $fatalOffset) {
                $newFatalText = Read-AppendedText -Path $FatalLogPath -Offset $fatalOffset
                $fatalOffset = $fatalLength
                if ($newFatalText -match '\[FATAL\]\[unhandled\]') {
                    Write-Output "[WATCHDOG][CRASH] pid=$watchedPid unhandled fatal exception recorded"
                    exit $ExitUnhandledFatal
                }
            }
        }
        catch {
            # A writer may briefly rotate or replace the file. Retry silently on the next poll.
        }
    }

    $target = Get-Process -Id $watchedPid -ErrorAction SilentlyContinue
    if ($null -eq $target) {
        Write-Output "[WATCHDOG][PROCESS_EXIT] pid=$watchedPid exited"
        exit $ExitProcessExited
    }

    if ($target.Responding) {
        $unresponsiveCount = 0
    }
    else {
        ++$unresponsiveCount
        if ($unresponsiveCount -ge $UnresponsivePolls) {
            Write-Output "[WATCHDOG][FREEZE] pid=$watchedPid Responding=False for $unresponsiveCount polls"
            exit $ExitUnresponsive
        }
    }

    if (-not $DisableHeartbeat -and (Test-Path -LiteralPath $LogPath)) {
        try {
            $logLength = (Get-Item -LiteralPath $LogPath).Length
            if ($logLength -ne $lastLogLength) {
                $lastLogLength = $logLength
                $lastHeartbeatUtc = [DateTime]::UtcNow
                $staleHeartbeatCount = 0
            }
            elseif (([DateTime]::UtcNow - $lastHeartbeatUtc).TotalSeconds -gt $HeartbeatTimeoutSeconds) {
                ++$staleHeartbeatCount
                if ($staleHeartbeatCount -ge $StaleHeartbeatPolls) {
                    Write-Output "[WATCHDOG][FREEZE] pid=$watchedPid trainer log heartbeat stale"
                    exit $ExitHeartbeatStale
                }
            }
        }
        catch {
            # File access errors are not themselves evidence of a game freeze. Retry silently.
        }
    }

    ++$pollCount
    if ($MaxPolls -gt 0 -and $pollCount -ge $MaxPolls) {
        exit 0
    }
    Start-Sleep -Milliseconds $PollMilliseconds
}
