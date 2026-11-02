# Deploys a locally built vmchroma64.dll and vm-sessions.exe to the workspace runtime trial and
# restarts the trial mixer (a few seconds of dropped audio). The DLL being replaced is kept beside
# it as vmchroma64.rollback.dll; run with -Rollback to put it back. Mixer routing and levels are
# compared before and after the restart. A restart drops any softphone call, so it refuses without
# -NoCallInProgress.
param(
    [switch]$NoCallInProgress,
    [string]$Trial = 'D:\Agent Workspace\VoicemeeterMod\runtime-trial',
    [switch]$Rollback
)
$ErrorActionPreference = 'Stop'
if (!$NoCallInProgress) { throw 'Refusing: this restarts Voicemeeter and would drop a call. Rerun with -NoCallInProgress when none is up.' }
$install = Join-Path ${env:ProgramFiles(x86)} 'VB\Voicemeeter'
$exe = Join-Path $Trial 'voicemeeter8x64_vmchroma.exe'
$dll = Join-Path $Trial 'vmchroma64.dll'
$saved = Join-Path $Trial 'vmchroma64.rollback.dll'
$newDll = Join-Path $PSScriptRoot 'source\emkaix-voicemeeter-chroma-dbae284\out\vmchroma64.dll'
$newPanel = Join-Path $PSScriptRoot 'build-tools\Release\vm-sessions.exe'
$newHelper = Join-Path $PSScriptRoot 'build-tools\Release\vm-softphone.exe'
$snapshot = Join-Path $PSScriptRoot 'Mixer-Snapshot.py'

foreach ($f in @($exe, (Join-Path $Trial 'voicemeeter8x64.exe'))) {
    if (!(Test-Path -LiteralPath $f)) { throw "Trial is missing $f (it needs the stock and patched EXE together)." }
}
if ($Rollback) {
    if (!(Test-Path -LiteralPath $saved)) { throw 'No rollback DLL saved.' }
} else {
    foreach ($f in @($newDll, $newPanel, $newHelper)) { if (!(Test-Path -LiteralPath $f)) { throw "Build first: $f is missing." } }
}

$before = Join-Path $env:TEMP 'vmchroma-before.json'
# An open App Sessions panel locks vm-sessions.exe; close it before anything is stopped.
$panelPath = Join-Path $Trial 'vm-sessions.exe'
foreach ($panel in @(Get-Process -Name vm-sessions -ErrorAction SilentlyContinue | Where-Object Path -eq $panelPath)) {
    [void]$panel.CloseMainWindow()
    if (!$panel.WaitForExit(5000)) { throw 'Close the App Sessions panel first; nothing was stopped.' }
}
$running = Get-Process -Name voicemeeter8x64_vmchroma -ErrorAction SilentlyContinue
if ($running) {
    if ($running.Path -ne $exe) { throw "Running Chroma is not the workspace trial ($($running.Path))." }
    python $snapshot save $before
    if ($LASTEXITCODE) { throw 'Could not snapshot the mixer.' }
    python $snapshot shutdown
    if (!$running.WaitForExit(20000)) { throw 'Mixer did not shut down within 20 s; nothing was replaced.' }
}

try {
    if ($Rollback) {
        Copy-Item -LiteralPath $saved -Destination $dll -Force
    } else {
        # Redeploying the same DLL (a helper-only update) must not overwrite the real rollback copy.
        if ((Get-FileHash -LiteralPath $dll).Hash -ne (Get-FileHash -LiteralPath $newDll).Hash) { Copy-Item -LiteralPath $dll -Destination $saved -Force }
        Copy-Item -LiteralPath $newDll -Destination $dll -Force
        Copy-Item -LiteralPath $newPanel -Destination $panelPath -Force
        # The softphone helper leaves with the mixer once it has told the host to go back to listen.
        foreach ($h in @(Get-Process -Name vm-softphone -ErrorAction SilentlyContinue)) { [void]$h.WaitForExit(5000) }
        Copy-Item -LiteralPath $newHelper -Destination (Join-Path $Trial 'vm-softphone.exe') -Force
    }
} finally {
    # Whatever happened above, never leave the user without a mixer.
    $proc = Start-Process -FilePath $exe -WorkingDirectory $install -PassThru
}
Start-Sleep -Seconds 6
$loaded = (Get-Process -Id $proc.Id).Modules | Where-Object ModuleName -eq 'vmchroma64.dll' | Select-Object -First 1
if (!$loaded -or $loaded.FileName -ne $dll) { throw "vmchroma64.dll from the trial is not loaded (got '$($loaded.FileName)')." }
Write-Output "Running PID $($proc.Id) with $((Get-FileHash -LiteralPath $dll).Hash)"
if ($running) {
    # The softphone helper mutes its send bus on startup, so that one difference is expected.
    $expected = if (Get-Process -Name vm-softphone -ErrorAction SilentlyContinue) { 'Bus[7].Mute' } else { @() }
    python $snapshot compare $before $expected
    if ($LASTEXITCODE) { throw 'Mixer state differs after the restart; see above.' }
    Remove-Item -LiteralPath $before
}
