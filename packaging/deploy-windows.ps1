#requires -Version 7.4
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$QtRoot,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$QtVersion = '6.8.3'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = Split-Path $PSScriptRoot -Parent
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '\bproject\s*\(\s*trans\s+VERSION\s+(\d+\.\d+\.\d+)\s') { throw 'Missing application version' }
$version = $Matches[1]
$output = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $output | Out-Null
$stage = Join-Path $output "trans-$version-windows-x64"
if (Test-Path $stage) { throw "Staging directory already exists: $stage" }
cmake --install $BuildDir --config Release --prefix $stage
$application = Join-Path $stage 'trans.exe'
$helper = Join-Path $stage 'trans_selection_helper.exe'
if (!(Test-Path $application) -or !(Test-Path $helper)) { throw 'Install with CMAKE_INSTALL_BINDIR=.' }
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-compiler-runtime --qmldir (Join-Path $root 'qml') $application $helper
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found for app-local runtime deployment' }
$crt = Get-Item (Join-Path $vs 'VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT') |
    Sort-Object FullName -Descending | Select-Object -First 1
if (!$crt) { throw 'MSVC x64 redistributable runtime not found' }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
@'
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
'@ | Set-Content -Encoding utf8 (Join-Path $stage 'qt.conf')
python (Join-Path $PSScriptRoot 'qt-notices.py') --version $QtVersion --output (Join-Path $stage 'licenses')
if (Test-Path (Join-Path $QtRoot 'sbom')) {
    Copy-Item -Recurse (Join-Path $QtRoot 'sbom') (Join-Path $stage 'licenses/qt-sbom')
}
# Prove deployed Qt resolution without the SDK on PATH or developer import paths.
$savedPath = $env:PATH
$savedPlugins = $env:QT_PLUGIN_PATH
$savedImports = $env:QML_IMPORT_PATH
$savedLegacyImports = $env:QML2_IMPORT_PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $null
    $env:QML_IMPORT_PATH = $null
    $env:QML2_IMPORT_PATH = $null
    $preview = Join-Path $output 'windows-settings-preview.png'
    $process = Start-Process $application -ArgumentList @('--smoke-test', '--screenshot', "`"$preview`"") -PassThru
    if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Packaged application startup timed out' }
    if ($process.ExitCode -ne 0 -or !(Test-Path $preview)) { throw "Packaged application smoke failed: $($process.ExitCode)" }
} finally {
    $env:PATH = $savedPath
    $env:QT_PLUGIN_PATH = $savedPlugins
    $env:QML_IMPORT_PATH = $savedImports
    $env:QML2_IMPORT_PATH = $savedLegacyImports
}
$zip = "$stage.zip"
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -Encoding ascii (Join-Path $output 'SHA256SUMS-windows-x64')
Write-Output "Packaged Trans ${version}: $zip"
