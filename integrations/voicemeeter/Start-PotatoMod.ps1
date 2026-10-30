param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$install = Join-Path ${env:ProgramFiles(x86)} 'VB\Voicemeeter'
$expected = [ordered]@{
    'voicemeeter8x64.exe' = '6F51C3E1B96E362AA68293BD4B9B74D6940FB08505DB1195990CC30A2185940B'
    'voicemeeter8x64_vmchroma.exe' = '91D66A4F2805AE8603D0C8D68C0B8E07BC29215DDF25B33C4325D661A2F54FFE'
    'vmchroma64.dll' = 'A6AC8652DCC9F015587E8386BC095D3865CE0D64850773557826B2F36E2A7A6C'
}
foreach ($name in $expected.Keys) {
    $file = Join-Path $install $name
    if ((Get-FileHash -LiteralPath $file).Hash -ne $expected[$name]) {
        throw "Compatibility check failed for $name. Use the stock Potato shortcut until this build has been checked."
    }
}
Write-Output 'Installed files match the Potato 3.1.1.8 / Chroma v0.3.1 trial.'
if ($CheckOnly) { return }
if (Get-Process -Name voicemeeter8x64_vmchroma -ErrorAction SilentlyContinue) {
    Write-Output 'Chroma is already running.'
    return
}
if (Get-Process -Name voicemeeter,voicemeeterx64,voicemeeterpro,voicemeeterpro_x64,voicemeeter8,voicemeeter8x64 -ErrorAction SilentlyContinue) {
    throw 'Close the running Voicemeeter edition from its tray menu before starting Chroma.'
}
Start-Process -FilePath (Join-Path $install 'voicemeeter8x64_vmchroma.exe') -WorkingDirectory $install
