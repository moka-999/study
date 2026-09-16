param(
    [ValidateSet('build', 'rebuild', 'check')]
    [string]$Task = 'build'
)

$ErrorActionPreference = 'Stop'
# 保留UTF-8 BOM，兼容Windows PowerShell 5读取中文注释。
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build_debug'
$cache = Join-Path $buildDir 'CMakeCache.txt'

function Get-CachedTool([string]$key) {
    if (Test-Path -LiteralPath $cache) {
        $pattern = '^' + [regex]::Escape($key) + ':[^=]+=(.+)$'
        $line = Select-String -LiteralPath $cache -Pattern $pattern | Select-Object -First 1
        if ($line) { return $line.Matches[0].Groups[1].Value }
    }
}

function Find-BundledExe([string]$bundle, [string]$exe, [string]$cacheKey) {
    # 已配置的绝对路径优先；由实际启动报告权限/文件错误，不因受限终端
    # 无法枚举安装目录而改选另一套工具链。无缓存才查PATH和标准bin目录。
    $cached = Get-CachedTool $cacheKey
    if ($cached) { return $cached }
    $command = Get-Command $exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $bundleRoot = Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$bundle"
    $versions = Get-ChildItem -LiteralPath $bundleRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName "bin\$exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw "STM32Cube tool not found: $exe"
}

function Find-Python {
    foreach ($key in @('Python3_EXECUTABLE', '_Python3_EXECUTABLE')) {
        $cached = Get-CachedTool $key
        if ($cached) { return $cached }
    }

    $codexPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $codexPython) { return $codexPython }
    $command = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($command -and $command.Source -notlike '*\WindowsApps\*') { return $command.Source }

    throw 'Python not found. Install Python 3 or add python.exe to PATH.'
}

$python = Find-Python
if ($Task -eq 'check') {
    # 都是离线检查，不打开串口、网络或调试器；缺失测试脚本必须报错。
    foreach ($script in @('verify_project.py', 'test_pc_power_protocol.py',
                          'test_manifest.py', 'test_legacy_flash.py')) {
        & $python -B -X utf8 (Join-Path $repo "tests\$script")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    exit 0
}

$ninja = Find-BundledExe 'ninja' 'ninja.exe' 'CMAKE_MAKE_PROGRAM'
$cmake = Find-BundledExe 'cmake' 'cmake.exe' 'CMAKE_COMMAND'

# 每次构建先轻量reconfigure，刷新UTC构建时间和76字节版本载荷。
# 这不是clean；Ninja仍只重编译受影响源码，工具链与已有缓存保持一致。
$cmakeArgs = @(
    '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Debug', "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DPython3_EXECUTABLE=$python"
)
if (-not (Test-Path -LiteralPath $cache)) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$repo\cmake\gcc-arm-none-eabi.cmake"
}
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Task -eq 'rebuild') {
    & $ninja -C $buildDir -t clean
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $ninja -C $buildDir
exit $LASTEXITCODE
