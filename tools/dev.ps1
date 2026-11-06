param(
    [ValidateSet('build', 'rebuild', 'check', 'flash')]
    [string]$Task = 'build',
    [ValidateSet('MCU', 'RK')]
    [string]$LogSource,
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateRange(1, 254)][int]$TestIpLastOctet,
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO'
)

$ErrorActionPreference = 'Stop'
# 保留UTF-8 BOM，兼容Windows PowerShell 5读取中文注释。
$repo = Split-Path -Parent $PSScriptRoot
# 日常快捷键始终输出到固定目录；显式末段沿用已有分板目录。
$buildName = $Configuration
$ipOverride = ''
if ($PSBoundParameters.ContainsKey('TestIpLastOctet')) {
    $ipOverride = [string]$TestIpLastOctet
    if ($TestIpLastOctet -ne 201) { $buildName += "-ip$TestIpLastOctet" }
}
$buildDir = Join-Path $repo "build/standalone/$buildName"
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

if ($Task -eq 'flash' -and $ConfirmIdle -ne 'YES') { throw '请先停止RK/温控并断开PC；允许复位后加 -ConfirmIdle YES。' }
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

# 每次构建先轻量reconfigure，刷新UTC构建时间和76字节版本载荷。
# 这不是clean；Ninja仍只重编译受影响源码，工具链与已有缓存保持一致。
$cmakeArgs = @(
    '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DPython3_EXECUTABLE=$python",
    "-DB1_TEST_IP_LAST_OCTET=$ipOverride"
)
$compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe' 'CMAKE_C_COMPILER'
$env:PATH = "$(Split-Path -Parent $compiler);$env:PATH"
if ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw 'Version格式为major.minor.patch。' }
    $parts = @($Matches[1], $Matches[2], $Matches[3])
    foreach ($part in $parts) { if ([long]$part -gt 65535) { throw '版本各段最大65535。' } }
    $cmakeArgs += @("-DFIRMWARE_VERSION_MAJOR=$($parts[0])", "-DFIRMWARE_VERSION_MINOR=$($parts[1])", "-DFIRMWARE_VERSION_PATCH=$($parts[2])")
}
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
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$image = Join-Path $buildDir 'b1.bin'
& $python -B -X utf8 (Join-Path $PSScriptRoot 'direct_flash.py') --image $image --target b1-h743 --check-only
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# 在镜像地址检查通过后导出带IP的完整ELF，方便两块B1分别烧录。
$testIp = (Get-Content -LiteralPath (Join-Path $buildDir 'b1_test_ip.txt') -Raw).Trim()
$namedElf = Join-Path $buildDir "b1_$testIp.elf"
Copy-Item -LiteralPath (Join-Path $buildDir 'b1.elf') -Destination $namedElf -Force
Write-Host "B1本地调试地址：${testIp}:8888；B2配置后仍可动态切换。"
Write-Host "完整Boot+App：$(Join-Path $buildDir 'b1.elf')、$image"
if ($Task -eq 'flash') {
    $download = @('-B', '-X', 'utf8', (Join-Path $PSScriptRoot 'direct_flash.py'),
        '--image', $image, '--target', 'b1-h743', '--confirm-idle', 'YES',
        '--programmer', (Find-BundledExe 'programmer' 'STM32_Programmer_CLI.exe' 'DIRECT_PROGRAMMER'))
    if ($Serial) { $download += @('--serial', $Serial) }
    & $python @download
    exit $LASTEXITCODE
}
exit 0
