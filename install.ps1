<#
  NewDarkVR one-step installer (Windows PowerShell).

  Builds everything from source, including the OpenXR loader, then optionally starts the stereo test.

      .\install.ps1          build
      .\install.ps1 -Run     build, then run the test

  You only need Python 3. The compiler (Zig) is installed from pip.
  If you just want to try it, download the zip from the Releases page instead and skip all of this.
#>
param([switch]$Run)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$py = $null
foreach ($c in "python", "py") {
    if (Get-Command $c -ErrorAction SilentlyContinue) { $py = $c; break }
}
if (-not $py) {
    Write-Host "Python 3 was not found."
    Write-Host "Install it from https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), then run this again."
    exit 1
}

Write-Host "Installing the Zig compiler (pip) ..."
& $py -m pip install --quiet --upgrade ziglang
if ($LASTEXITCODE -ne 0) { throw "pip could not install ziglang" }

& $py build.py
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$rt = (Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1" -ErrorAction SilentlyContinue).ActiveRuntime
if ($rt) {
    Write-Host "Active OpenXR runtime: $rt"
} else {
    Write-Host ""
    Write-Host "No active OpenXR runtime is registered yet. In the Meta Quest Link app, open"
    Write-Host "Settings > General and set Meta Quest Link as the OpenXR runtime."
}

if ($Run) {
    Write-Host "Starting the stereo test. Put the headset on."
    Start-Process -FilePath (Join-Path $PSScriptRoot "build\ndvr_gltest.exe") -WorkingDirectory (Join-Path $PSScriptRoot "build")
} else {
    Write-Host ""
    Write-Host "Built. Run build\ndvr_gltest.exe (or re-run with -Run)."
}
