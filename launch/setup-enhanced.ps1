param(
    [Parameter(Mandatory=$true)][string]$Iso,
    [Parameter(Mandatory=$true)][string]$Work,
    [string]$Pcsx2Source,
    [string]$Pcsx2Dependencies,
    [ValidateRange(1,32)][int]$Jobs=4
)
$ErrorActionPreference='Stop'
$checkout=(Resolve-Path "$PSScriptRoot/..").Path
$private=[IO.Path]::GetFullPath($Work)
if($private -eq $checkout -or $private.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Work must be outside the source checkout' }
$disc=(Resolve-Path -LiteralPath $Iso).Path
$pins=Get-Content -LiteralPath "$checkout/deps/hwgs/dependencies.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $private | Out-Null
if(-not $env:VCToolsInstallDir) {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if(-not $vs) { throw 'MSVC x64 Build Tools are required' }
    & "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
$env:PYTHONDONTWRITEBYTECODE='1'
$env:PS2X_BUILD_JOBS=[string]$Jobs
if(-not $Pcsx2Source) {
    $Pcsx2Source=Join-Path $private 'dependencies/pcsx2'
    if(-not (Test-Path -LiteralPath $Pcsx2Source)) {
        New-Item -ItemType Directory -Path $Pcsx2Source | Out-Null
        & git -C $Pcsx2Source init -q
        if($LASTEXITCODE -ne 0) { throw 'git init failed' }
        & git -C $Pcsx2Source config core.longpaths true
        & git -C $Pcsx2Source remote add origin $pins.pcsx2_url
        & git -C $Pcsx2Source fetch --depth 1 origin $pins.pcsx2_revision
        if($LASTEXITCODE -ne 0) { throw 'PCSX2 source fetch failed' }
        & git -C $Pcsx2Source checkout --detach FETCH_HEAD
        if($LASTEXITCODE -ne 0) { throw 'PCSX2 checkout failed' }
    }
}
$Pcsx2Source=(Resolve-Path -LiteralPath $Pcsx2Source).Path
$revision=& git -C $Pcsx2Source rev-parse HEAD
if($LASTEXITCODE -ne 0 -or $revision -ne $pins.pcsx2_revision) { throw 'PCSX2 source does not match the pinned revision' }
& git -C $Pcsx2Source diff --quiet HEAD
if($LASTEXITCODE -ne 0) { throw 'Use an unmodified PCSX2 dependency checkout' }
if(-not $Pcsx2Dependencies) {
    $depWork=Join-Path $private 'dependencies/pcsx2-windows'
    $archive=Join-Path $private 'dependencies/pcsx2-windows-dependencies.7z'
    $stamp=Join-Path $depWork 'archive-sha256.txt'
    if(-not (Test-Path -LiteralPath $stamp)) {
        if(Test-Path -LiteralPath $depWork) { throw 'Incomplete dependency extraction: retain it for inspection and use a fresh Work directory' }
        if(-not (Test-Path -LiteralPath $archive)) {
            New-Item -ItemType Directory -Force -Path (Split-Path $archive -Parent) | Out-Null
            Invoke-WebRequest -Uri $pins.windows_dependencies_url -OutFile "$archive.pending"
            if((Get-FileHash -LiteralPath "$archive.pending" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pins.windows_dependencies_sha256) {
                throw 'Downloaded dependency bundle changed; the pinned SHA-256 did not match'
            }
            Move-Item -LiteralPath "$archive.pending" -Destination $archive
        }
        if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pins.windows_dependencies_sha256) { throw 'Cached dependency archive hash mismatch' }
        $seven=Get-Command 7z -ErrorAction SilentlyContinue
        $sevenPath=if($seven){$seven.Source}else{Join-Path $env:ProgramFiles '7-Zip/7z.exe'}
        if(-not (Test-Path -LiteralPath $sevenPath)) { throw '7-Zip is required to extract the dependency bundle' }
        & $sevenPath x $archive "-o$depWork" -y
        if($LASTEXITCODE -ne 0) { throw 'Dependency extraction failed' }
        [IO.File]::WriteAllText($stamp,$pins.windows_dependencies_sha256)
    }
    if((Get-Content -LiteralPath $stamp -Raw).Trim() -ne $pins.windows_dependencies_sha256) { throw 'Dependency extraction belongs to another bundle' }
    $Pcsx2Dependencies=Join-Path $depWork 'deps'
}
$Pcsx2Dependencies=(Resolve-Path -LiteralPath $Pcsx2Dependencies).Path
if(-not (Test-Path -LiteralPath "$Pcsx2Dependencies/include/zstd.h")) { throw 'Incomplete PCSX2 Windows dependency directory' }
# Git subprocesses used by CMake need long-path support too, without changing global Git settings.
$env:GIT_CONFIG_COUNT='1';$env:GIT_CONFIG_KEY_0='core.longpaths';$env:GIT_CONFIG_VALUE_0='true'
& python -B "$checkout/setup/setup.py" --iso $disc --work "$private/port" --cmake-arg=-DPS2X_WITH_HWGS=ON "--cmake-arg=-DKZGS_PCSX2_DIR=$Pcsx2Source" "--cmake-arg=-DKZGS_DEPS_DIR=$Pcsx2Dependencies" --cmake-arg=-DKZGS_ISA=AVX2
if($LASTEXITCODE -ne 0) { throw 'Port setup/build failed; the private logs and generation can be resumed' }
