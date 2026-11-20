# Stages the Windows app and builds the portable zip and the Inno Setup installer.
#
#   pwsh installer/build-installer.ps1 -Version 1.0.0 [-BuildDir build] [-OutDir dist]
#
# Output in OutDir:
#   DAW-v<Version>-windows-x64.zip   portable (unzip and run)
#   DAW-Setup-v<Version>.exe         installer
param(
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $BuildDir = "build",
    [string] $OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$exe = Join-Path $root "$BuildDir/DAW_artefacts/Release/DAW.exe"
if (-not (Test-Path $exe)) { throw "DAW.exe not found at $exe (build the Release configuration first)" }

$asioLicense = Join-Path $root "$BuildDir/_deps/juce-src/modules/juce_audio_devices/native/asio/LICENSE.txt"
if (-not (Test-Path $asioLicense)) { throw "ASIO SDK license not found at $asioLicense" }

# --- Stage
$out = Join-Path $root $OutDir
$stage = Join-Path $out "DAW"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $exe $stage
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item $asioLicense (Join-Path $stage "THIRD-PARTY-ASIO-LICENSE.txt")

# --- Portable zip
$zip = Join-Path $out "DAW-v$Version-windows-x64.zip"
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force

# --- Installer (Inno Setup 6: preinstalled on GitHub's Windows images; installed if missing)
$iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) { $iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) {
    choco install innosetup -y --no-progress
    $iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
}
if (-not (Test-Path $iscc)) { throw "Inno Setup compiler (ISCC.exe) not found" }

& $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutputDir=$out" (Join-Path $PSScriptRoot "DAW.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$setup = Join-Path $out "DAW-Setup-v$Version.exe"
if (-not (Test-Path $setup)) { throw "installer not found at $setup" }

foreach ($file in @($zip, $setup)) {
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash
    Write-Host ("{0}  {1}  {2:N0} bytes" -f $hash, (Split-Path -Leaf $file), (Get-Item $file).Length)
}
