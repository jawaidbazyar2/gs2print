$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Error "Install Build Tools for Visual Studio 2022 with the Desktop development with C++ workload. The Visual Studio IDE is not required."
}

$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) {
    Write-Error "The MSVC toolset was not found. Install Build Tools for Visual Studio 2022 and select Desktop development with C++."
}

$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
$cmakeBin = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
if (Test-Path $ninjaBin) { $env:PATH = "$ninjaBin;$env:PATH" }

$build = Join-Path $Root 'build'
$tests = Join-Path $build 'gs2print_tests.exe'
cmd /c "call `"$vcvars`" && cmake -S `"$Root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$build`" && `"$tests`""
exit $LASTEXITCODE
