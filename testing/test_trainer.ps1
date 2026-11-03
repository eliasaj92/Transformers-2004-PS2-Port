param([Parameter(Mandatory=$true)][string]$Work)
$ErrorActionPreference='Stop'
$checkout=(Resolve-Path "$PSScriptRoot/..").Path
$output=[IO.Path]::GetFullPath($Work)
if ($output -eq $checkout -or $output.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Test products must be outside sources' }
if(Test-Path -LiteralPath $output) { throw 'Use a fresh private test directory' }
New-Item -ItemType Directory -Path $output | Out-Null
Push-Location $output
try {
    & cl /nologo /std:c++20 /EHsc /W4 /MD "/I$PSScriptRoot/trainer_host" "/I$checkout/PS2Recomp/ps2xRuntime/include" "$PSScriptRoot/trainer_host/test_trainer.cpp" "$checkout/PS2Recomp/ps2xRuntime/src/lib/ps2_trainer.cpp" /Fe:trainer-test.exe
    if($LASTEXITCODE -ne 0) { throw 'Trainer test compile failed' }
    & ./trainer-test.exe "$output/fixture"
    if($LASTEXITCODE -ne 0) { throw 'Trainer behavior tests failed' }
} finally { Pop-Location }
