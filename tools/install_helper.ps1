# Installs the PC helper in your user folder, starts it at login and (re)starts it now. Run it again after changing the
# helper. Your settings file is kept.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Target = Join-Path $HOME 'HoustonHelper'
$Helper = Join-Path $Target 'houston_helper.py'
$TaskName = 'Houston helper'

New-Item -ItemType Directory -Force $Target | Out-Null

# Only this helper, found by its command line: other programs run Python too
$Running = Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*\HoustonHelper\houston_helper.py*' }
foreach ($Process in $Running) { Stop-Process -Id $Process.ProcessId -Force }

Copy-Item (Join-Path $Root 'pc_helper\houston_helper.py'), (Join-Path $Root 'pc_helper\calendar_ics.py') $Target -Force

$Config = Join-Path $Target 'houston.local.json'
if (-not (Test-Path $Config)) { Copy-Item (Join-Path $Root 'pc_helper\config.example.json') $Config }

$Python = & python -c "import sys; print(sys.executable)"
$Pythonw = Join-Path (Split-Path -Parent $Python) 'pythonw.exe'
if (-not (Test-Path $Pythonw)) { throw "pythonw.exe not found next to $Python" }

$Action = New-ScheduledTaskAction -Execute $Pythonw -Argument "`"$Helper`"" -WorkingDirectory $Target
$Trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$TaskSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger -Settings $TaskSettings -Description 'Sends GPU, print, Claude Code and calendar state to Houston' -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output "Installed in $Target and started; task '$TaskName' starts it at login. Settings: $Config"
