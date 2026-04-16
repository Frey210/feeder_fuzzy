Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$researchRoot = Join-Path $repoRoot "feeder-research"
$venvPath = Join-Path $researchRoot ".venv"
$pythonExe = Join-Path $venvPath "Scripts\python.exe"
$requirementsPath = Join-Path $researchRoot "requirements.txt"

if (-not (Test-Path $venvPath)) {
    py -3 -m venv $venvPath
}

& $pythonExe -m pip install --upgrade pip
& $pythonExe -m pip install -r $requirementsPath

Write-Host ""
Write-Host "Environment ready:"
Write-Host "  $pythonExe"
Write-Host ""
Write-Host "Activate with:"
Write-Host "  $venvPath\Scripts\Activate.ps1"
