param(
    [ValidateSet('build', 'rebuild', 'flash', 'style-test', 'protocol-test', 'b2-protocol-test', 'pc-protocol-test', 'pc-net-test-build', 'pwr-test', 'lpddr-test', 'boot-test', 'pwr-wave-build', 'led-test', 'led-wave-build', 'flash-test', 'flash-self-test-build', 'driver-mock-build', 'ad-wave-build')]
    [string]$Task = 'build',
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO',
    [switch]$PowerSimulation,
    [switch]$MatsFailTest
)

$ErrorActionPreference = 'Stop'
if (($PowerSimulation -or $MatsFailTest) -and $Task -notin @('build', 'rebuild', 'flash')) {
    throw '电源模拟/假FAIL选项仅允许普通构建或直接下载。'
}
if ($MatsFailTest -and -not $PowerSimulation) { throw '-MatsFailTest 必须同时指定 -PowerSimulation。' }
if ($Task -eq 'flash' -and $ConfirmIdle -ne 'YES') { throw '请先停止测试并断开PC；允许复位后加 -ConfirmIdle YES。' }
# 本文件保留UTF-8 BOM，避免Windows PowerShell 5按本地编码误读中文注释。
$repo = Split-Path -Parent $PSScriptRoot
$buildDirName = if ($Task -eq 'pwr-wave-build') {
    'build_pwr_wave'
} elseif ($Task -eq 'led-wave-build') {
    'build_led_wave'
} elseif ($Task -eq 'flash-self-test-build') {
    'build_flash_self_test'
} elseif ($Task -eq 'driver-mock-build') {
    'build_driver_mock'
} elseif ($Task -eq 'ad-wave-build') {
    'build_ad_wave'
} elseif ($Task -eq 'pc-net-test-build') {
    'build_pc_net_test'
} else {
    "build/standalone/$Configuration"
}
$buildDir = Join-Path $repo $buildDirName

function Find-BundledExe([string]$bundle, [string]$exe) {
    $root = Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$bundle"
    # STM32Cube bundle 的标准结构是 <版本>/bin/<exe>，先走确定路径，避免
    # 对带完整文档的 CMake 目录做数十秒递归扫描。
    $versionDirs = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    if ($versionDirs) {
        # 交给实际启动报告“文件不存在”；某些受限终端会让 Test-Path 对可执行
        # 文件偶发返回 False，不能因此把已安装工具误判为缺失。
        return Join-Path $versionDirs[0].FullName "bin\$exe"
    }

    # 兼容少数不遵循标准目录结构的旧 bundle。
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

if ($Task -eq 'style-test') {
    $python = Find-Python
    & $python -B -X utf8 (Join-Path $repo 'tests\verify_code_boundaries.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # 不提供HAL/FreeRTOS/cJSON搜索路径，验证纯接口头不泄漏实现依赖。
    $compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe'
    if (-not $compiler) { throw 'STM32Cube GCC not found.' }
    & $compiler -std=c11 -Wall -Wextra -Werror -fsyntax-only `
        "-I$repo\HAL_Drv" "-I$repo\App\protocol\pc" "-I$repo\App\protocol\b2" `
        (Join-Path $repo 'tests\c\portable_headers.c')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output 'Portable headers: GCC syntax check PASS (no HAL/RTOS/cJSON headers)'
    exit 0
}

if ($Task -eq 'protocol-test' -or $Task -eq 'b2-protocol-test' -or $Task -eq 'pc-protocol-test' -or $Task -eq 'pwr-test' -or $Task -eq 'lpddr-test' -or $Task -eq 'boot-test' -or
    $Task -eq 'led-test' -or $Task -eq 'flash-test') {
    $python = Find-Python
    $script = if ($Task -eq 'pc-protocol-test') {
        'tests\verify_pc_protocol.py'
    } elseif ($Task -eq 'b2-protocol-test') {
        'tests\verify_b2_protocol_c.py'
    } elseif ($Task -eq 'boot-test') {
        'tests\test_boot_mode.py'
    } elseif ($Task -eq 'lpddr-test') {
        'tests\verify_lpddr_power.py'
    } elseif ($Task -eq 'pwr-test') {
        'tests\verify_pwr_ctrl.py'
    } elseif ($Task -eq 'led-test') {
        'tests\verify_result_led.py'
    } elseif ($Task -eq 'flash-test') {
        'tests\test_version.py'
    } else {
        'tests\verify_rk_protocol_c.py'
    }
    & $python -B -X utf8 (Join-Path $repo $script)
    exit $LASTEXITCODE
}

$ninja = $null
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    # 首次由命令行指定 Ninja 时，CMake 可能把类型记为 UNINITIALIZED；
    # 两种缓存类型都应复用，避免受当前 PowerShell PATH 或沙箱可见性影响。
    $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_MAKE_PROGRAM:(?:FILEPATH|UNINITIALIZED)=(.+)$' | Select-Object -First 1
    if ($line) { $ninja = $line.Matches[0].Groups[1].Value }
}
if (-not $ninja) {
    $ninja = Find-BundledExe 'ninja' 'ninja.exe'
}
# 受限终端可能无法枚举STM32Cube工具目录。
# 此时复用既有正式构建缓存中的工具绝对路径。
if (-not $ninja) {
    $referenceCache = Join-Path $repo 'build_debug\CMakeCache.txt'
    if (Test-Path -LiteralPath $referenceCache) {
        $line = Select-String -LiteralPath $referenceCache -Pattern '^CMAKE_MAKE_PROGRAM:(?:FILEPATH|UNINITIALIZED)=(.+)$' | Select-Object -First 1
        if ($line) { $ninja = $line.Matches[0].Groups[1].Value }
    }
}
if (-not $ninja) { throw 'STM32Cube Ninja not found.' }

# 每次正式构建都快速重新配置一次，用当前UTC时间刷新固件版本元数据。
# 版本宏只属于 flash_layout.c，因此通常只重编译该文件并重新链接。
$cmake = Find-BundledExe 'cmake' 'cmake.exe'
if (-not $cmake) {
    $referenceCache = Join-Path $repo 'build_debug\CMakeCache.txt'
    if (Test-Path -LiteralPath $referenceCache) {
        $line = Select-String -LiteralPath $referenceCache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
        if ($line) { $cmake = $line.Matches[0].Groups[1].Value }
    }
}
if (-not $cmake) { throw 'STM32Cube CMake not found.' }
$pwrWaveOption = if ($Task -eq 'pwr-wave-build') { '-DENABLE_PWR_WAVE_TEST=ON' } else { '-DENABLE_PWR_WAVE_TEST=OFF' }
$ledWaveOption = if ($Task -eq 'led-wave-build') { '-DENABLE_RESULT_LED_WAVE_TEST=ON' } else { '-DENABLE_RESULT_LED_WAVE_TEST=OFF' }
$flashSelfTestOption = if ($Task -eq 'flash-self-test-build') { '-DENABLE_FLASH_SELF_TEST=ON' } else { '-DENABLE_FLASH_SELF_TEST=OFF' }
$driverMockOption = if ($Task -eq 'driver-mock-build') { '-DENABLE_DRIVER_MOCK_TEST=ON' } else { '-DENABLE_DRIVER_MOCK_TEST=OFF' }
$adWaveOption = if ($Task -eq 'ad-wave-build') { '-DENABLE_AD7124_WAVE_TEST=ON' } else { '-DENABLE_AD7124_WAVE_TEST=OFF' }
$pcNetTestOption = if ($Task -eq 'pc-net-test-build') { '-DENABLE_PC_PROTOCOL_NET_TEST=ON' } else { '-DENABLE_PC_PROTOCOL_NET_TEST=OFF' }
$cmakeArgs = @(
    '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    $pwrWaveOption,
    $ledWaveOption,
    $flashSelfTestOption
    $driverMockOption
    $adWaveOption
    $pcNetTestOption
    '-DENABLE_RK_GATEWAY_TEST=OFF'
    "-DENABLE_V2_POWER_SIMULATION=$(if ($PowerSimulation) { 'ON' } else { 'OFF' })"
    "-DENABLE_V2_MATS_FAIL_TEST=$(if ($MatsFailTest) { 'ON' } else { 'OFF' })"
)
$python = Find-Python
$compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe'
$env:PATH = "$(Split-Path -Parent $compiler);$env:PATH"
$cmakeArgs += "-DPython3_EXECUTABLE=$python"
if ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw 'Version格式为major.minor.patch。' }
    $parts = @($Matches[1], $Matches[2], $Matches[3])
    foreach ($part in $parts) { if ([long]$part -gt 65535) { throw '版本各段最大65535。' } }
    $cmakeArgs += @("-DFIRMWARE_VERSION_MAJOR=$($parts[0])", "-DFIRMWARE_VERSION_MINOR=$($parts[1])", "-DFIRMWARE_VERSION_PATCH=$($parts[2])")
}
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
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$image = Join-Path $buildDir 'test.bin'
& $python -B -X utf8 (Join-Path $PSScriptRoot 'direct_flash.py') --image $image --target apollo-v2-h743 --check-only
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Task -eq 'flash') {
    $download = @('-B', '-X', 'utf8', (Join-Path $PSScriptRoot 'direct_flash.py'),
        '--image', $image, '--target', 'apollo-v2-h743', '--confirm-idle', 'YES',
        '--programmer', (Find-BundledExe 'programmer' 'STM32_Programmer_CLI.exe'))
    if ($Serial) { $download += @('--serial', $Serial) }
    & $python @download
    exit $LASTEXITCODE
}
exit 0
