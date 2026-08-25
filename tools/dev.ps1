param(
    [ValidateSet('build', 'rebuild', 'protocol-test', 'pc-protocol-test', 'pc-net-test-build', 'pwr-test', 'lpddr-test', 'boot-test', 'pwr-wave-build', 'led-test', 'led-wave-build', 'flash-test', 'flash-self-test-build', 'driver-mock-build', 'ad-wave-build')]
    [string]$Task = 'build'
)

$ErrorActionPreference = 'Stop'
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
    'build_debug'
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

if ($Task -eq 'protocol-test' -or $Task -eq 'pc-protocol-test' -or $Task -eq 'pwr-test' -or $Task -eq 'lpddr-test' -or $Task -eq 'boot-test' -or
    $Task -eq 'led-test' -or $Task -eq 'flash-test') {
    $python = Find-Python
    $script = if ($Task -eq 'pc-protocol-test') {
        'tests\verify_pc_protocol.py'
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
        'tests\verify_protocol_v2.py'
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
# A restricted shell may not enumerate the STM32Cube bundle directory.
# Reuse the absolute tool path from the configured production build cache.
if (-not $ninja) {
    $referenceCache = Join-Path $repo 'build_debug\CMakeCache.txt'
    if (Test-Path -LiteralPath $referenceCache) {
        $line = Select-String -LiteralPath $referenceCache -Pattern '^CMAKE_MAKE_PROGRAM:(?:FILEPATH|UNINITIALIZED)=(.+)$' | Select-Object -First 1
        if ($line) { $ninja = $line.Matches[0].Groups[1].Value }
    }
}
if (-not $ninja) { throw 'STM32Cube Ninja not found.' }

# 每次正式构建都快速重新配置一次，用当前 UTC/Git 状态刷新固件版本元数据。
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
    '-DCMAKE_BUILD_TYPE=Debug',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    $pwrWaveOption,
    $ledWaveOption,
    $flashSelfTestOption
    $driverMockOption
    $adWaveOption
    $pcNetTestOption
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
