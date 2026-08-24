param(
    [ValidateSet('build', 'rebuild', 'protocol-test', 'pwr-test', 'pwr-wave-build', 'led-test', 'led-wave-build', 'flash-test', 'flash-self-test-build')]
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

if ($Task -eq 'protocol-test' -or $Task -eq 'pwr-test' -or
    $Task -eq 'led-test' -or $Task -eq 'flash-test') {
    $python = Find-Python
    $script = if ($Task -eq 'pwr-test') {
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
if (-not $ninja) { throw 'STM32Cube Ninja not found.' }

# 每次正式构建都快速重新配置一次，用当前 UTC/Git 状态刷新固件版本元数据。
# 版本宏只属于 flash_layout.c，因此通常只重编译该文件并重新链接。
$cmake = Find-BundledExe 'cmake' 'cmake.exe'
if (-not $cmake) { throw 'STM32Cube CMake not found.' }
$pwrWaveOption = if ($Task -eq 'pwr-wave-build') { '-DENABLE_PWR_WAVE_TEST=ON' } else { '-DENABLE_PWR_WAVE_TEST=OFF' }
$ledWaveOption = if ($Task -eq 'led-wave-build') { '-DENABLE_RESULT_LED_WAVE_TEST=ON' } else { '-DENABLE_RESULT_LED_WAVE_TEST=OFF' }
$flashSelfTestOption = if ($Task -eq 'flash-self-test-build') { '-DENABLE_FLASH_SELF_TEST=ON' } else { '-DENABLE_FLASH_SELF_TEST=OFF' }
$cmakeArgs = @(
    '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Debug',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    $pwrWaveOption,
    $ledWaveOption,
    $flashSelfTestOption
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
