$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17,18)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install VS 2022 C++ Build Tools with the Windows SDK and CMake.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$build = Join-Path (Split-Path $PSScriptRoot) 'build-tools'
& $cmake -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)." }
& $cmake --build $build --config Release --parallel 4
if ($LASTEXITCODE) { throw "Tool compilation failed ($LASTEXITCODE)." }
Get-ChildItem (Join-Path $build 'Release\*.exe') | Get-FileHash
