param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}

$vs = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vs) {
    throw "Visual Studio C++ build tools not found."
}

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = "cmake" }

$build = Join-Path $root "build"

$cmd = "call `"$vcvars`" >nul && " +
       "`"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && " +
       "`"$cmake`" --build `"$build`""

& cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Built: $(Join-Path $build 'FolderViz.exe')" -ForegroundColor Green
