#requires -Version 7.4
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$DependenciesRoot,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$SlintSourceDir,
    [string]$RustToolchain = '1.98.1'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = Split-Path $PSScriptRoot -Parent
$build = [IO.Path]::GetFullPath($BuildDir)
$dependencies = [IO.Path]::GetFullPath($DependenciesRoot)
if (!$SlintSourceDir) { $SlintSourceDir = Join-Path $build '_deps/slint-src' }
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '\bproject\s*\(\s*trans\s+VERSION\s+(\d+\.\d+\.\d+)\s') { throw 'Missing application version' }
$version = $Matches[1]
$output = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $output | Out-Null
$stage = Join-Path $output "trans-$version-windows-x64"
if (Test-Path $stage) { throw "Staging directory already exists: $stage" }
# Install only application runtime artifacts, never the Slint compiler, SDK,
# import libraries or headers that the upstream subproject also installs.
cmake --install $build --config Release --component Runtime --prefix $stage
$application = Join-Path $stage 'trans.exe'
$helper = Join-Path $stage 'trans_selection_helper.exe'
if (!(Test-Path $application) -or !(Test-Path $helper)) {
    throw 'Configure with "-DCMAKE_INSTALL_BINDIR=." and "-DCMAKE_INSTALL_LIBDIR=." for portable Windows packaging.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found for app-local runtime deployment' }
$toolsVersion = (Get-Content (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $vs "VC/Tools/MSVC/$toolsVersion/bin/Hostx64/x64/dumpbin.exe"
if (!(Test-Path $dumpbin)) { throw 'MSVC x64 dumpbin not found' }
$crt = Get-Item (Join-Path $vs 'VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT') |
    Sort-Object FullName -Descending | Select-Object -First 1
if (!$crt) { throw 'MSVC x64 redistributable runtime not found' }
# The redistributable set includes STL satellite DLLs loaded dynamically, not
# necessarily enumerated by a single PE's ordinary import table.
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
$searchDirectories = @(
    (Join-Path $build 'Release'), $build, (Join-Path $dependencies 'bin'),
    (Join-Path $build '_deps/slint-build/Release'), (Join-Path $build '_deps/slint-build'), $crt.FullName
) | Where-Object { Test-Path $_ -PathType Container }
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem $stage -File | Where-Object Extension -in '.exe', '.dll' | ForEach-Object { $queue.Enqueue($_.FullName) }
$inspected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$systemDlls = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($queue.Count) {
    $binary = $queue.Dequeue()
    if (!$inspected.Add($binary)) { continue }
    $headers = (& $dumpbin /NOLOGO /HEADERS $binary) -join "`n"
    if ($headers -notmatch '(?im)^\s*8664 machine \(x64\)') { throw "Not an x64 PE binary: $binary" }
    $imports = & $dumpbin /NOLOGO /DEPENDENTS $binary
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { continue }
        $name = $Matches[1]
        if ($name -match '^(?:lib)?(?:Qt[0-9]*|KF[0-9]*)|^q(?:windows|xcb|wayland)') {
            throw "Forbidden GUI dependency in ${binary}: $name"
        }
        $destination = Join-Path $stage $name
        if (Test-Path $destination -PathType Leaf) { $queue.Enqueue($destination); continue }
        if ($name -match '^(api-ms-win-|ext-ms-win-)') { $null = $systemDlls.Add($name); continue }
        $candidates = @($searchDirectories | ForEach-Object {
            $candidate = Join-Path $_ $name
            if (Test-Path $candidate -PathType Leaf) { [IO.Path]::GetFullPath($candidate) }
        } | Select-Object -Unique)
        if ($candidates.Count) {
            $hashes = @($candidates | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash } | Select-Object -Unique)
            if ($hashes.Count -ne 1) { throw "Conflicting runtime DLL versions for ${name}: $($candidates -join ', ')" }
            Copy-Item $candidates[0] $destination
            $queue.Enqueue($destination)
        } elseif (Test-Path (Join-Path "$env:SystemRoot/System32" $name) -PathType Leaf) {
            $null = $systemDlls.Add($name)
        } else {
            throw "Unresolved runtime DLL imported by ${binary}: $name"
        }
    }
}
if (!(Test-Path (Join-Path $stage 'slint_cpp.dll'))) { throw 'Slint runtime DLL was not installed alongside trans.exe' }
$licenses = Join-Path $stage 'licenses'
python (Join-Path $PSScriptRoot 'runtime-notices.py') --slint-source $SlintSourceDir --target x86_64-pc-windows-msvc --rust-toolchain $RustToolchain --output $licenses
# vcpkg installs each port's original, complete copyright/license collection,
# including notices for bundled third-party source. Retain all installed ports,
# including static/header-only libraries and build tools, conservatively.
$portNotices = @(Get-ChildItem (Join-Path $dependencies 'share/*/copyright') -File)
if (!$portNotices.Count) { throw 'No vcpkg dependency licenses found' }
foreach ($notice in $portNotices) {
    $port = $notice.Directory.Name
    if ($port -match '^(qt[0-9]?|kf[0-9]?)') { throw "Unexpected forbidden dependency installed: $port" }
    $destination = Join-Path $licenses "native/$port"
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item $notice.FullName (Join-Path $destination 'copyright')
}
foreach ($required in 'curl', 'libpng', 'zlib', 'nlohmann-json') {
    if (!(Test-Path (Join-Path $licenses "native/$required/copyright"))) { throw "Missing dependency license: $required" }
}
$crtNotices = Join-Path $licenses 'microsoft-vc-runtime'
New-Item -ItemType Directory -Force $crtNotices | Out-Null
$crtLicense = 'https://visualstudio.microsoft.com/wp-content/uploads/2021/09/Visual-C-Runtime-2015-2022-License-1.docx'
Invoke-WebRequest -Uri $crtLicense -OutFile (Join-Path $crtNotices 'Visual-C-Runtime-2015-2022-License.docx')
@(
    'Microsoft Visual C++ 2015-2022 Runtime, x64 app-local redistributable files.',
    "Runtime version: $($crt.Parent.Parent.Name)",
    'Original license terms: https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/',
    "Original license document: $crtLicense"
) | Set-Content -Encoding utf8 (Join-Path $crtNotices 'provenance.txt')
$revision = (git -C $root rev-parse HEAD).Trim()
@(
    "Trans $version", "Source revision: $revision", "Repository: $env:GITHUB_REPOSITORY",
    "Slint: 1.18.1, Winit/FemtoVG/accessibility/system-tray; Rust $RustToolchain",
    'Native networking: libcurl with Windows Schannel; native image encoding: libpng.',
    'Third-party license expressions retain their upstream alternatives; they do not grant a license to Trans source.',
    "Build environment: MSVC $toolsVersion / Windows x64"
) | Set-Content -Encoding utf8 (Join-Path $stage 'BUILD-INFO.txt')
$systemDlls | Sort-Object | Set-Content -Encoding utf8 (Join-Path $stage 'SYSTEM-DLLS.txt')
# Exercise the distributed binaries with no developer SDK or dependency paths.
# --smoke-test opens both Slint windows and exits; screenshot export is not part
# of this application's smoke contract.
$savedPath = $env:PATH
$savedBackend = $env:SLINT_BACKEND
$savedConfig = $env:SLINT_STYLE
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:SLINT_BACKEND = 'winit-femtovg'
    $env:SLINT_STYLE = $null
    $process = Start-Process $application -ArgumentList '--smoke-test' -WorkingDirectory $stage -PassThru
    if (!$process.WaitForExit(30000)) { $process.Kill($true); $process.WaitForExit(); throw 'Packaged application startup timed out' }
    if ($process.ExitCode -ne 0) { throw "Packaged application smoke failed: $($process.ExitCode)" }
} finally {
    $env:PATH = $savedPath
    $env:SLINT_BACKEND = $savedBackend
    $env:SLINT_STYLE = $savedConfig
}
$inventory = @(Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
        Path = [IO.Path]::GetRelativePath($stage, $_.FullName)
        Bytes = $_.Length
        SHA256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$inventory | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $output 'windows-runtime-files.csv')
$unpackedBytes = ($inventory | Measure-Object Bytes -Sum).Sum
$zip = "$stage.zip"
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -Encoding ascii (Join-Path $output 'SHA256SUMS-windows-x64')
Write-Output "Packaged Trans ${version}: $zip"
Write-Output "Complete unpacked bytes: $unpackedBytes; ZIP bytes: $((Get-Item $zip).Length)"
