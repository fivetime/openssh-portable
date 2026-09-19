# NativeTerm's local build of Win32-OpenSSH (x64 Release), for machines where
# the repo's own Start-OpenSSHBuild doesn't work: it only knows Visual Studio
# 2015-2022 (by "2022" in MSBuild's path), and relies on Visual Studio's vcpkg
# integration being on. Nothing outside this build is changed: no
# `vcpkg integrate install`, no edits to tracked files.
#
# Needs: Visual Studio 2022 or 2026 with the v143 x64/x86 build tools and their
# Spectre-mitigated libraries; a vcpkg clone next to this repo (..\vcpkg).
#
#   .\nativeterm\build.ps1            dependencies (once), then the build
#   .\nativeterm\build.ps1 -SkipDeps  the build only
param([switch]$SkipDeps)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot
$sln = Join-Path $repo 'contrib\win32\openssh'
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path (Split-Path $repo) 'vcpkg' }
$vcpkg = Join-Path $vcpkgRoot 'vcpkg.exe'
$triplet = 'x64-custom'
# the projects look for vcpkg_installed\<triplet>\<triplet>\...
$installed = Join-Path $sln "vcpkg_installed\$triplet"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ build tools not found' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'

# the newest Windows 10/11 SDK installed (the repo pins one in paths.targets)
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory |
    Where-Object { $_.Name -like '10.*' } | Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty Name

if (-not $SkipDeps) {
    if (-not (Test-Path $vcpkg)) { & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics }
    $env:VCPKG_VISUAL_STUDIO_PATH = $vs
    Push-Location $sln
    try {
        & $vcpkg install --triplet $triplet "--overlay-triplets=$PSScriptRoot\vcpkg_triplets" `
            --overlay-ports=.\vcpkg_overlay_ports "--x-install-root=$installed"
        if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}

$props = Join-Path $vcpkgRoot 'scripts\buildsystems\msbuild\vcpkg.props'
$targets = Join-Path $vcpkgRoot 'scripts\buildsystems\msbuild\vcpkg.targets'
& $msbuild (Join-Path $sln 'Win32-OpenSSH.sln') /t:Build /p:Platform=x64 /p:Configuration=Release `
    "/p:WindowsSDKVersion=$sdk" "/p:ForceImportBeforeCppProps=$props" "/p:ForceImportAfterCppTargets=$targets" `
    /p:VcpkgManifestInstall=false "/p:VcpkgInstalledDir=$installed\" /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built: $(Join-Path $repo 'bin\x64\Release\ssh.exe')"
