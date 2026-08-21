param(
    [ValidateSet('build', 'rebuild', 'check')]
    [string]$Task = 'build'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build_debug'

function Find-BundledExe([string]$bundle, [string]$exe) {
    $root = Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$bundle"
    Get-ChildItem -LiteralPath $root -Filter $exe -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

if ($Task -eq 'check') {
    $python = (Get-Command python.exe -ErrorAction SilentlyContinue).Source
    if (-not $python) { throw 'Python not found.' }
    & $python -B -X utf8 (Join-Path $repo 'tests\verify_project.py')
    exit $LASTEXITCODE
}

$ninja = Find-BundledExe 'ninja' 'ninja.exe'
$cmake = Find-BundledExe 'cmake' 'cmake.exe'
$gcc = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe'
if (-not $ninja -or -not $cmake -or -not $gcc) {
    throw 'STM32Cube CMake, Ninja or GNU Arm toolchain not found.'
}
$env:PATH = "$(Split-Path -Parent $gcc);$env:PATH"

if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) {
    & $cmake -S $repo -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$repo\cmake\gcc-arm-none-eabi.cmake" `
        -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_MAKE_PROGRAM=$ninja"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Task -eq 'rebuild') {
    & $ninja -C $buildDir -t clean
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $ninja -C $buildDir
exit $LASTEXITCODE
