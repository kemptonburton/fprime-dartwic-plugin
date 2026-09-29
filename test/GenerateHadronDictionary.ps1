param(
    [string]$HadronRoot = (Join-Path $PSScriptRoot '..\..\Hadron-FSW')
)

$ErrorActionPreference = 'Stop'
$HadronRoot = (Resolve-Path -LiteralPath $HadronRoot).Path
$fppToDict = Join-Path $HadronRoot 'fprime-venv\Scripts\fpp-to-dict.exe'
if (-not (Test-Path -LiteralPath $fppToDict)) {
    throw "Windows fpp-to-dict.exe is missing: $fppToDict"
}
$output = Join-Path $HadronRoot 'build-local\fprime-ccsds-dictionary'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Push-Location $HadronRoot
try {
    $imports = @(rg --files lib/fprime HadronFSW | Where-Object {
        $_ -match '\.fpp$' -and
        $_ -notmatch '(\\test\\|TestDeployment|SITLDeployment|\\Ref\\|\\examples\\|\\Apps\\)'
    })
    if ($LASTEXITCODE -ne 0 -or $imports.Count -eq 0) {
        throw 'Could not enumerate the F Prime FPP imports.'
    }
    & $fppToDict -i ($imports -join ',') -d $output `
        'HadronFSW/SITLDeployment/Top/instances.fpp' `
        'HadronFSW/SITLDeployment/Top/topology.fpp'
    if ($LASTEXITCODE -ne 0) { throw 'F Prime dictionary generation failed.' }
    Write-Output (Join-Path $output 'SITLDeploymentTopologyDictionary.json')
} finally {
    Pop-Location
}
