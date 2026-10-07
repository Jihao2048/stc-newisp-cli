# Smoke-test the Windows build.
#
# Nothing here needs hardware: every case exercises argument handling, device
# enumeration or an error path. Run it after a successful win-build.ps1.

$ErrorActionPreference = 'Continue'

$bin = Join-Path $env:USERPROFILE 'newisp-build-win\Release\newisp.exe'
if (-not (Test-Path $bin)) {
    Write-Error "not built: $bin"
}

$fail = 0

function Run-Case {
    param([string[]]$Arguments)
    Write-Host ('=' * 62)
    Write-Host ("$ newisp " + ($Arguments -join ' '))
    Write-Host ('-' * 62)
    & $bin @Arguments
    $code = $LASTEXITCODE
    Write-Host "--- exit code: $code ---"
    Write-Host ''
}

Run-Case @('--version')
Run-Case @('list')
Run-Case @('detect', '--device', 'COM99')
Run-Case @('burn')
Run-Case @('burn', '-f', 'C:\nonexistent.hex', '--device', 'COM99')
Run-Case @('burn', '-f', 'C:\nonexistent.hex', '--baud', 'notanumber')
Run-Case @('nosuchcommand')

Write-Host ('=' * 62)
Write-Host 'all smoke cases executed'
