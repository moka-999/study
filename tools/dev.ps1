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

function Find-Python {
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $codexPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $codexPython) { return $codexPython }

    throw 'Python not found. Install Python 3 or add python.exe to PATH.'
}

if ($Task -eq 'check') {
    $python = Find-Python
    & $python -B -X utf8 (Join-Path $repo 'tests\verify_project.py')
    exit $LASTEXITCODE
}

$ninja = $null
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_MAKE_PROGRAM:(?:FILEPATH|UNINITIALIZED)=(.+)$' | Select-Object -First 1
    if ($line) { $ninja = $line.Matches[0].Groups[1].Value }
}
if (-not $ninja -or -not (Test-Path -LiteralPath $ninja)) {
    $ninja = Find-BundledExe 'ninja' 'ninja.exe'
}
if (-not $ninja) { throw 'STM32Cube Ninja not found.' }

if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) {
    $cmake = Find-BundledExe 'cmake' 'cmake.exe'
    if (-not $cmake) { throw 'STM32Cube CMake not found.' }
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
