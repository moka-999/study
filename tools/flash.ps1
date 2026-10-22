param(
    [ValidateSet('build', 'deploy', 'flash', 'verify')][string]$Action = 'flash',
    [ValidateSet('apollo-v2', 'b1')][string]$Board = '',
    [string]$AppRoot = '',
    [string]$Bundle = $PSScriptRoot,
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet(2, 4)][int]$RkUart = 4,
    [switch]$PowerSimulation,
    [switch]$MatsFailTest,
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO',
    [ValidateSet('NO', 'YES')][string]$ConfirmBlank = 'NO',
    [string]$Python = '',
    [string]$Compiler = '',
    [string]$Server = '',
    [string]$Programmer = '',
    [string]$CMake = '',
    [string]$Ninja = ''
)

$ErrorActionPreference = 'Stop'
# 保留UTF-8 BOM以兼容Windows PowerShell 5；优先显式路径，其次PATH和STM32Cube工具包。
function Find-Tool([string]$Explicit, [string]$Name, [string]$BundleName) {
    if ($Explicit) { return $Explicit }
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found -and $found.Source -notlike '*\WindowsApps\*') { return $found.Source }
    if ($BundleName) {
        $versions = Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$BundleName") -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($versions) { return Join-Path $versions[0].FullName "bin\$Name" }
    }
    if ($Name -eq 'python.exe') {
        $candidate = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "找不到 $Name；请安装工具、加入PATH或使用对应参数指定路径。"
}

if (-not $Board -and $Action -eq 'verify') {
    $Board = (Get-Content -LiteralPath (Join-Path $Bundle 'manifest.json') -Raw | ConvertFrom-Json).board
}
if ($Board -notin @('apollo-v2', 'b1')) { throw '请明确 -Board apollo-v2 或 -Board b1；无法通过芯片ID自动识别PCB板型。' }
$needsBuild = $Action -in @('build', 'deploy')
$needsFlash = $Action -in @('flash', 'deploy')
if (-not $needsBuild -and ($Version -or $PowerSimulation -or $MatsFailTest)) { throw '烧写/校验使用包内版本与配置，不能重新设置构建参数。' }
if (($Board -eq 'b1' -and ($PowerSimulation -or $MatsFailTest)) -or ($MatsFailTest -and -not $PowerSimulation)) {
    throw 'MATS假FAIL仅用于V2电源模拟配置；B1禁止模拟和假FAIL。'
}
if ($needsFlash -and ($ConfirmIdle -ne 'YES' -or $ConfirmBlank -ne 'YES' -or $Serial -notmatch '^[0-9a-fA-F]{24}$')) {
    throw '烧写须指定24位ST-Link序列号，并设置 -ConfirmIdle YES -ConfirmBlank YES；先确认空板及RK/温控空闲。'
}
$Python = Find-Tool $Python 'python.exe' ''
$script = if ($needsBuild) { Join-Path $PSScriptRoot 'first_flash.py' } else { Join-Path $Bundle 'first_flash.py' }
$arguments = @('-B', '-X', 'utf8', $script, $Action, '--board', $Board)
if ($needsBuild) {
    if (-not $AppRoot -or -not $Version) { throw '构建须指定AppRoot和三段Version。' }
    $arguments += @('--app-root', $AppRoot, '--version', $Version, '--configuration', $Configuration,
        '--rk-uart', $RkUart, '--compiler', (Find-Tool $Compiler 'arm-none-eabi-gcc.exe' 'gnu-tools-for-stm32'),
        '--cmake', (Find-Tool $CMake 'cmake.exe' 'cmake'), '--ninja', (Find-Tool $Ninja 'ninja.exe' 'ninja'))
    if ($PowerSimulation) { $arguments += '--power-simulation' }
    if ($MatsFailTest) { $arguments += '--mats-fail-test' }
} else {
    $arguments += @('--bundle', $Bundle)
}
if ($needsFlash) {
    # deploy直接使用本次构建的精确目录；不扫描旧包，不在构建前连接设备。
    $arguments += @('--serial', $Serial, '--confirm-idle', '--confirm-blank',
        '--server', (Find-Tool $Server 'ST-LINK_gdbserver.exe' 'stlink-gdbserver'),
        '--programmer', (Find-Tool $Programmer 'STM32_Programmer_CLI.exe' 'programmer'))
    if (-not $needsBuild) { $arguments += @('--compiler', (Find-Tool $Compiler 'arm-none-eabi-gcc.exe' 'gnu-tools-for-stm32')) }
}
& $Python @arguments
exit $LASTEXITCODE
