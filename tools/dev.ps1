param(
    [ValidateSet('build', 'rebuild', 'ota-test', 'ota-pack')]
    [string]$Task = 'build',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('apollo-v2', 'b1')]
    [string]$Board = 'apollo-v2',
    [ValidateSet('None', 'A1', 'A2')]
    [string]$ProbeSlot = 'None',
    [string]$AppBuild = '',
    [string]$Qemu = 'D:\qemu\qemu-system-arm.exe'
)

$ErrorActionPreference = 'Stop'
$bootRoot = Split-Path -Parent $PSScriptRoot

# 复用已安装的STM32Cube工具链，不安装新依赖、不访问烧录器。
function Find-BundledExe([string]$bundle, [string]$exe) {
    $bundleRoot = Join-Path $env:LOCALAPPDATA "stm32cube\bundles\$bundle"
    $versionDir = Get-ChildItem -LiteralPath $bundleRoot -Directory |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $versionDir) { throw "STM32Cube bundle not found: $bundle" }
    return Join-Path $versionDir.FullName "bin\$exe"
}

$compiler = Find-BundledExe 'gnu-tools-for-stm32' 'arm-none-eabi-gcc.exe'
$cmake = Find-BundledExe 'cmake' 'cmake.exe'
$ninja = Find-BundledExe 'ninja' 'ninja.exe'
$env:PATH = (Split-Path -Parent $compiler) + ';' + $env:PATH

# 测试与打包仍使用原GNU工具链；Python/QEMU只在PC离线运行，不连SWD。
if ($Task -eq 'ota-test' -or $Task -eq 'ota-pack') {
    if ($ProbeSlot -ne 'None') { throw 'ProbeSlot is only valid for build/rebuild' }
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    if ($Task -eq 'ota-pack') {
        if (-not $AppBuild) { throw 'ota-pack requires -AppBuild pointing to verified A1 build' }
        $appName = if ($Board -eq 'b1') { 'b1' } else { 'test' }
        & $python -X utf8 (Join-Path $bootRoot 'tools\pack_ota.py') --app-build $AppBuild --compiler $compiler --app-name $appName
    } else {
        $testArgs = @('-X', 'utf8', (Join-Path $bootRoot 'tests\verify_ota_install.py'),
            '--compiler', $compiler, '--cmake', $cmake, '--ninja', $ninja,
            '--configuration', $Configuration, '--qemu', $Qemu)
        if ($AppBuild) { $testArgs += @('--app-build', $AppBuild) }
        & $python @testArgs
    }
    exit $LASTEXITCODE
}
$buildName = $Configuration
if ($Board -eq 'b1') { $buildName += '-b1' }
if ($ProbeSlot -ne 'None') { $buildName += "-$ProbeSlot" }
$bootBuild = Join-Path $bootRoot "build\$buildName"
$probe = if ($ProbeSlot -eq 'None') { 'OFF' } else { 'ON' }
$appBase = if ($ProbeSlot -eq 'A2') { '0x08100000' } else { '0x08020000' }

& $cmake -S $bootRoot -B $bootBuild -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_TOOLCHAIN_FILE=$bootRoot\cmake\gcc-arm-none-eabi.cmake" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DENABLE_OTA_RELOCATION_PROBE=$probe" `
    "-DOTA_APP_BASE=$appBase" "-DBOOT_BOARD=$Board"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$buildArgs = @('--build', $bootBuild)
if ($Task -eq 'rebuild') { $buildArgs += '--clean-first' }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 最小产物回归：B必须位于128 KiB以内，初始栈和Thumb入口必须与链接布局相符。
# 这里只检查生成的B镜像，不代表B已具备对外部升级包的完整安全校验。
$bootBytes = [IO.File]::ReadAllBytes((Join-Path $bootBuild 'boot.bin'))
if ($bootBytes.Length -lt 8 -or $bootBytes.Length -gt 131072) { throw 'Invalid B image size' }
$bootSp = [BitConverter]::ToUInt32($bootBytes, 0)
$bootEntry = [BitConverter]::ToUInt32($bootBytes, 4)
if ($bootSp -ne 0x20020000 -or ($bootEntry -band 1) -ne 1 -or
    ($bootEntry - 1) -lt 0x08000008 -or
    ($bootEntry - 1) -ge (0x08000000 + $bootBytes.Length)) {
    throw 'Invalid B reset vector'
}
Write-Output "PASS: $bootBuild\boot.elf / boot.bin; B vector and size checked. No flashing."
