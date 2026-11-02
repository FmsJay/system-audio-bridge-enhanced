# Moves this PC from the floating "Softphone jump-in" window to the softphone built into the Potato mod.
# Run only with James's OK at that moment and no call up: it restarts Voicemeeter. In order:
#   1. reports (never removes) any Laptop-jump shortcut in shell:startup
#   2. closes the floating window gracefully so its own cleanup runs
#   3. checks that nothing else holds the hotkey
#   4. turns the per-PC setting on in vmchroma.yaml, keeping a backup
#   5. deploys the build and restarts Voicemeeter through Deploy-Trial.ps1
# To go back: restore the yaml backup this prints, run Deploy-Trial.ps1 -NoCallInProgress -Rollback, set Bus[7].Mute=0, start Laptop-jump.cmd.
param(
    [switch]$NoCallInProgress,
    [string]$HostIp = '100.106.240.32',   # JAMES-MAIN on Tailscale; its Command1 slot filters on our Tailscale source address
    [string]$StateFile = '\\JAMES-MAIN\Portable Workstation\Claude Agent Workspace\audio-bridge\live-state.txt',
    [string]$Hotkey = 'F9'
)
$ErrorActionPreference = 'Stop'
if (!$NoCallInProgress) { throw 'Refusing: this restarts Voicemeeter and would drop a call. Rerun with -NoCallInProgress when James has approved it.' }

$startup = [Environment]::GetFolderPath('Startup')
$shell = New-Object -ComObject WScript.Shell
foreach ($link in Get-ChildItem -LiteralPath $startup -Filter *.lnk) {
    $target = $shell.CreateShortcut($link.FullName).TargetPath
    if ("$($link.Name) $target" -match 'Laptop-jump') { Write-Output "NOTE for James: '$($link.Name)' in shell:startup still starts the floating window. Left in place; remove it when ready." }
}

$jump = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'" | Where-Object CommandLine -match 'Laptop-jump')
foreach ($p in $jump) {
    $process = Get-Process -Id $p.ProcessId
    [void]$process.CloseMainWindow()
    if (!$process.WaitForExit(10000)) { throw "The Softphone jump-in window (PID $($p.ProcessId)) did not close; nothing else was changed." }
    Write-Output "Closed the floating window (PID $($p.ProcessId))."
}

Add-Type -Namespace SoftphoneCutover -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint key); [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);'
$vk = 0x6F + [int]$Hotkey.Substring(1)   # F1 = 0x70
if (![SoftphoneCutover.Native]::RegisterHotKey([IntPtr]::Zero, 991, 0x4000, $vk)) { throw "$Hotkey is held by something else, so it can't be the mic key. Nothing else was changed." }
[void][SoftphoneCutover.Native]::UnregisterHotKey([IntPtr]::Zero, 991)
Write-Output "$Hotkey is free."

$yaml = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Voicemeeter\vmchroma.yaml'
$text = [IO.File]::ReadAllText($yaml)
if ($text -match '(?m)^softphone:') { throw "$yaml already has a softphone section; edit it by hand." }
$backup = "$yaml.before-softphone"
Copy-Item -LiteralPath $yaml -Destination $backup
$section = "softphone:`n  enabled: true`n  hostIp: `"$HostIp`"`n  stateFile: '$StateFile'`n  hotkey: `"$Hotkey`"`n  sendBus: 7`n  micStrip: 0`n"
[IO.File]::WriteAllText($yaml, $text.TrimEnd() + "`n" + $section)
Write-Output "Setting on in $yaml (backup: $backup)."

& (Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'Deploy-Trial.ps1') -NoCallInProgress
Start-Sleep -Seconds 2
$helper = Get-Process -Name vm-softphone -ErrorAction SilentlyContinue
Write-Output ("vm-softphone running: " + [bool]$helper)
