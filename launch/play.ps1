param(
    [Parameter(Mandatory=$true)][string]$GameFolder,
    [ValidateSet('normal','trainer')][string]$Profile,
    [ValidateSet('pgs','hwgs','cpu')][string]$Backend,
    [ValidateSet('direct','readback')][string]$Presentation
)
$ErrorActionPreference='Stop'
$folder=(Resolve-Path $GameFolder).Path
$disc=Get-Content (Join-Path $folder 'disc.json') -Raw | ConvertFrom-Json
foreach($line in Get-Content "$PSScriptRoot/play_env.txt") {
    if ($line -match '^([A-Z0-9_]+)=(.*)$') {
        # These options belong to the persisted menu; CLI/environment overrides still win.
        if ($Matches[1] -notin @('PS2X_GS_BACKEND','PS2X_UPSCALE')) {
            [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
        }
    }
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
