# selfhost.ps1 — run pal.codes from this machine, exposed via Tailscale Funnel.
#
#   .\scripts\selfhost.ps1 install     register + start the background server (runs at logon)
#   .\scripts\selfhost.ps1 restart     recompile from src/ and restart the server
#   .\scripts\selfhost.ps1 status      is it running, and is the funnel up?
#   .\scripts\selfhost.ps1 uninstall   stop it and remove the task
#
# Content updates don't need any of this: `.\palsite publish` rebuilds and the
# running server reloads within a second.
#
# The server runs its own static build in .selfhost\, so you can keep
# recompiling ./palsite.exe while the live site is up; `restart` rebuilds it
# from src/ and swaps it in.
param(
    [Parameter(Mandatory)][ValidateSet('install', 'restart', 'status', 'uninstall', 'run')]
    [string]$Action,
    [int]$Port = 8080
)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Dir  = Join-Path $Root '.selfhost'
$Exe  = Join-Path $Dir 'palsite.exe'
$Log  = Join-Path $Dir 'serve.log'
$Err  = Join-Path $Dir 'serve.err.log'
$Task = 'palsite'

function Stop-Server {
    if (Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $Task
    }
    Get-Process palsite -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $Exe } |
        Stop-Process -Force -Confirm:$false
}

# Compile a *static* binary for the server. The default build links MinGW's
# libstdc++ DLLs, and outside Git Bash's PATH Windows can load a mismatched
# copy (exit 0xC0000139). Static also means nothing else can break it later.
function Build-Binary {
    if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) { throw 'g++ not found on PATH' }
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $src = Get-ChildItem (Join-Path $Root 'src') -Filter *.cpp | ForEach-Object FullName
    & g++ -std=c++17 -O2 -static -o $Exe @src -lws2_32
    if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
}

function Start-Server {
    Start-ScheduledTask -TaskName $Task
    Start-Sleep -Seconds 1
    try {
        $r = Invoke-WebRequest "http://127.0.0.1:$Port/" -UseBasicParsing -TimeoutSec 3
        "server up on 127.0.0.1:$Port ($($r.StatusCode))"
    } catch {
        Write-Warning "server didn't answer on port $Port; see $Log"
    }
}

switch ($Action) {
    'install' {
        Stop-Server
        Build-Binary
        & $Exe publish -C $Root | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'build failed' }

        # At logon, a hidden PowerShell runs this script's `run` action, which
        # supervises the server. No admin rights or stored password needed.
        $ps = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" run -Port $Port"
        $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $ps -WorkingDirectory $Root
        $trigger    = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
        $settings   = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                          -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
        Register-ScheduledTask -TaskName $Task -Action $taskAction -Trigger $trigger `
            -Settings $settings -Description 'pal.codes static server (palsite serve)' -Force | Out-Null
        Start-Server
    }
    'restart' {
        Stop-Server
        Build-Binary
        Start-Server
    }
    'status' {
        $t = Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue
        if (-not $t) { 'not installed'; break }
        "task:   $($t.State)"
        $l = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
        "listen: $(if ($l) { "127.0.0.1:$Port (pid $($l[0].OwningProcess))" } else { 'nothing on port ' + $Port })"
        if (Get-Command tailscale -ErrorAction SilentlyContinue) {
            ''
            tailscale funnel status
        }
        ''
        "log ($Log):"
        if (Test-Path $Log) { Get-Content $Log -Tail 5 }
        if ((Test-Path $Err) -and (Get-Item $Err).Length) { "errors ($Err):"; Get-Content $Err -Tail 5 }
    }
    'run' {
        # Supervisor (started by the task): keep the server up, restart on crash.
        while ($true) {
            $p = Start-Process $Exe -ArgumentList "serve -C `"$Root`" -p $Port" -WorkingDirectory $Root `
                     -WindowStyle Hidden -RedirectStandardOutput $Log -RedirectStandardError $Err -PassThru
            $p.WaitForExit()
            Start-Sleep -Seconds 5
        }
    }
    'uninstall' {
        Stop-Server
        if (Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $Task -Confirm:$false
        }
        'removed. (the funnel is separate: `tailscale funnel reset` to take it down)'
    }
}
