# Configure and build newisp on Windows.
#
# Visual Studio's developer environment is entered explicitly (vcvars64.bat)
# rather than assumed, so this works from any shell. The CMake used is the one
# installed by pip for the bundled Python, because Visual Studio does not always
# ship a copy and ninja is not guaranteed to be present either -- hence the
# "Visual Studio 17 2022" generator, which needs nothing but MSBuild.

$ErrorActionPreference = 'Stop'

$repo   = 'C:\cmdisp\newisp-cross'
$build  = Join-Path $env:USERPROFILE 'newisp-build-win'
$vs     = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$python = 'C:\Program Files (x86)\Microsoft Visual Studio\Shared\Python39_64\python.exe'

if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
}

Write-Host '=== locate cmake ==='
$cmake = & $python -c "import cmake, os; print(os.path.join(os.path.dirname(cmake.__file__), 'data', 'bin', 'cmake.exe'))"
if (-not (Test-Path $cmake)) {
    Write-Error "cmake not found via python module: $cmake"
}
Write-Host "cmake: $cmake"

# The vcvars environment cannot be imported into an existing PowerShell process,
# so the whole build runs inside one cmd.exe that has it applied.
$script = @"
call "$vcvars" >nul
echo === configure ===
"$cmake" -S "$repo" -B "$build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
echo.
echo === build ===
"$cmake" --build "$build" --config Release
if errorlevel 1 exit /b 1
echo.
echo === result ===
dir /b "$build\Release\newisp.exe"
"@

$tmp = Join-Path $env:TEMP 'newisp-win-build.cmd'
Set-Content -Path $tmp -Value $script -Encoding ASCII

cmd.exe /c $tmp
exit $LASTEXITCODE
