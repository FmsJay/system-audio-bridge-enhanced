$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'source\emkaix-voicemeeter-chroma-dbae284'
$build = Join-Path $PSScriptRoot 'build-x64'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17,18)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install VS 2022 C++ Build Tools with the Windows SDK and CMake.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --version
& $cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)." }
& $cmake --build $build --config Release --target vmchroma --parallel 4
if ($LASTEXITCODE) { throw "Chroma compilation failed ($LASTEXITCODE)." }
Get-FileHash -LiteralPath (Join-Path $source 'out\vmchroma64.dll')
