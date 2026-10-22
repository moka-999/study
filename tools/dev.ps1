param(
    [ValidateSet('build', 'rebuild', 'check', 'ota-build', 'ota-swd')]
    [string]$Task = 'build',
    [ValidateSet('MCU', 'RK')]
    [string]$LogSource,
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO'
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
    $compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe' 'CMAKE_C_COMPILER'
    & $python -B -X utf8 (Join-Path $repo 'tests\verify_board_identity.py') --compiler $compiler
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $python -B -X utf8 (Join-Path $repo 'tests\verify_power_sequence.py') --compiler $compiler
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}

$ninja = Find-BundledExe 'ninja' 'ninja.exe' 'CMAKE_MAKE_PROGRAM'
$cmake = Find-BundledExe 'cmake' 'cmake.exe' 'CMAKE_COMMAND'

if ($Task -eq 'ota-swd') {
    # 固定B1板型，不接受V2模拟；快捷键传YES即代表操作者已确认停测及允许复位。
    if ($ConfirmIdle -ne 'YES') { throw '请先停止RK/温控并断开PC，确认后加-ConfirmIdle YES。' }
    if ($LogSource) { throw 'ota-swd日志默认来源沿用B1工程配置，不接收-LogSource。' }
    if (-not $Version) { $Version = 'auto' }
    $compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe' 'CMAKE_C_COMPILER'
    $server = Find-BundledExe 'stlink-gdbserver' 'ST-LINK_gdbserver.exe' 'OTA_STLINK_SERVER'
    $programmer = Find-BundledExe 'programmer' 'STM32_Programmer_CLI.exe' 'OTA_PROGRAMMER'
    $shared = Join-Path (Split-Path -Parent $repo) 'test\tools\ota_dev.py'
    if (-not (Test-Path -LiteralPath $shared)) { throw 'B1开发侧一键下载需要同级test工程。' }
    $arguments = @('-B', '-X', 'utf8', $shared, 'swd', '--board', 'b1', '--app-root', $repo,
        '--version', $Version, '--configuration', $Configuration, '--confirm-idle',
        '--cmake', $cmake, '--ninja', $ninja, '--compiler', $compiler,
        '--server', $server, '--programmer', $programmer)
    if ($Serial) { $arguments += @('--serial', $Serial) }
    & $python @arguments
    exit $LASTEXITCODE
}

if ($Task -eq 'ota-build') {
    # 只构建B1发布包及独立网络工具，不烧录；实际升级须操作者另行执行。
    if (-not $Version) { throw 'ota-build需要-Version，例如0.1.37；须高于板上版本。' }
    if ($LogSource) { throw 'ota-build不接收-LogSource；日志默认来源沿用B1工程配置。' }
    $compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe' 'CMAKE_C_COMPILER'
    & $python -B -X utf8 (Join-Path $repo 'tools\ota_build.py') --version $Version `
        --configuration $Configuration --cmake $cmake --ninja $ninja --compiler $compiler
    exit $LASTEXITCODE
}

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
if ($LogSource) {
    # 仅改变本次构建缓存中的上电默认值；运行时log命令仍然可用。
    $logSourceName = $LogSource.ToUpperInvariant()
    $cmakeArgs += "-DB1_LOG_DEFAULT_SOURCE=$logSourceName"
}
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Task -eq 'rebuild') {
    & $ninja -C $buildDir -t clean
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $ninja -C $buildDir
exit $LASTEXITCODE
