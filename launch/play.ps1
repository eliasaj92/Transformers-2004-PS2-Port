param(
    [Parameter(Mandatory=$true)][string]$GameFolder,
    [ValidateSet('normal','trainer')][string]$Profile,
    [ValidateSet('pgs','hwgs','cpu')][string]$Backend,
    [ValidateSet('direct','readback')][string]$Presentation,
    [switch]$Enhanced
)
$ErrorActionPreference='Stop'
$folder=(Resolve-Path $GameFolder).Path
$disc=Get-Content (Join-Path $folder 'disc.json') -Raw | ConvertFrom-Json
if ($Enhanced) {
    if (($Backend -and $Backend -ne 'hwgs') -or ($Presentation -and $Presentation -ne 'direct')) {
        throw 'The combined enhanced preset requires hardware GS with direct presentation'
    }
    $build=Get-Content -LiteralPath (Join-Path $folder 'local-build.json') -Raw | ConvertFrom-Json
    if (-not $disc.static_vu1 -or -not $build.gpu_geometry_runtime_bypass_available -or
        -not $build.gpu_grass_runtime_bypass_available -or -not $build.gpu_reflection_runtime_bypass_available -or
        -not $build.frame_ready_wait) {
        throw 'This private build does not contain the combined enhanced runtime and presenter'
    }
    foreach ($item in @(@('ps2EntryRunner.exe',$build.exe_sha256), @('tf_hwgs.dll',$build.dll_sha256))) {
        if ((Get-FileHash -LiteralPath (Join-Path $folder $item[0]) -Algorithm SHA256).Hash -ne $item[1]) {
            throw "Enhanced build provenance does not match $($item[0])"
        }
    }
    $Backend='hwgs'; $Presentation='direct'
}
foreach($line in Get-Content "$PSScriptRoot/play_env.txt") {
    if ($line -match '^([A-Z0-9_]+)=(.*)$') {
        # These options belong to the persisted menu; CLI/environment overrides still win.
        if ($Matches[1] -notin @('PS2X_GS_BACKEND','PS2X_UPSCALE')) {
            [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
        }
    }
}
if ($Enhanced) {
    foreach ($line in Get-Content -LiteralPath "$PSScriptRoot/enhanced_env.txt") {
        if ($line -match '^([A-Z0-9_]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
        }
    }
    # Play never inherits an earlier diagnostic capture/profiling session.
    foreach ($key in @('PS2X_TF_FRAME_PROFILE','PS2X_TF_SCENE_CAPTURE','PS2X_TF_SCENE_RANGE',
                      'PS2X_PRESENT_TRACE','PS2X_FIELD60_LOG','PS2X_TF_TIMING_TRACE',
                      'PS2X_TF_PRESENT_TRACE','PS2X_TF_GPU_DRAW_TRACE','PS2X_PCMENU_AUTOTEST',
                      'PS2X_RESOPT2_HOST_SEQUENCE')) {
        [Environment]::SetEnvironmentVariable($key,$null,'Process')
    }
    [Diagnostics.Process]::GetCurrentProcess().PriorityClass='Normal'
}
$env:PS2X_CD_IMAGE=$disc.iso
if ($Presentation) {
    if ($Backend -and $Backend -ne 'hwgs') { throw 'Presentation selection requires -Backend hwgs' }
    $Backend='hwgs'
    $env:PS2X_HWGS_DIRECT=if ($Presentation -eq 'direct') {'1'} else {'0'}
    $env:PS2X_HWGS_API=if ($Presentation -eq 'direct') {'d3d12'} else {'vulkan'}
}
if ($disc.static_vu1) { $env:PS2X_VU1_AOT='1'; $env:PS2X_VU1_AOT_STATIC='1' }
if (-not $env:PS2X_SCRIPT) {
    if (-not $disc.mc_root) { throw 'Rerun setup to configure a persistent memory card' }
    $env:PS2X_MC_ROOT=$disc.mc_root
    if (-not $env:PS2X_SETTINGS) {
        $env:PS2X_SETTINGS=Join-Path (Split-Path $disc.mc_root -Parent) 'settings.ini'
    }
    if ($env:PS2X_SETTINGS -ne 'none' -and -not (Test-Path -LiteralPath $env:PS2X_SETTINGS)) {
        $initialSettings=@('[Renderer]','backend=pgs','hw_scale=4','hw_fxaa=off','','[Display]','scaling_filter=bilinear','','[Trainer]','profile=normal') -join "`n"
        [IO.File]::WriteAllText($env:PS2X_SETTINGS,$initialSettings+"`n",[Text.UTF8Encoding]::new($false))
    }
    & python "$PSScriptRoot/../setup/cards.py" --disc (Join-Path $folder 'disc.json')
    if ($LASTEXITCODE -ne 0) { throw 'Memory card backup failed' }
    $trainerCard=Join-Path (Split-Path $disc.mc_root -Parent) 'profiles/trainer/mc0'
    if (Test-Path -LiteralPath $trainerCard) {
        & python -B -c 'import sys;sys.path.insert(0,sys.argv[1]);from cards import backup;backup(sys.argv[2])' "$PSScriptRoot/../setup" $trainerCard
        if ($LASTEXITCODE -ne 0) { throw 'Trainer memory card backup failed' }
    }
}
$runnerArgs=@($disc.elf)
if ($Profile) { $runnerArgs+=@('--set',"trainer.profile=$Profile") }
if ($Backend) { $runnerArgs+=@('--set',"renderer.backend=$Backend") }
Push-Location $folder
try { & (Join-Path $folder 'ps2EntryRunner.exe') @runnerArgs; exit $LASTEXITCODE } finally { Pop-Location }
