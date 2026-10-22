param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
# 仅编译第二块裸机板，不枚举或连接ST-Link。
$python = Get-Command python.exe -ErrorAction SilentlyContinue
if ($python) {
    $runtime = $python.Source
} else {
    $runtime = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
}
if (-not $Compiler) {
    $bundle = Join-Path $env:LOCALAPPDATA 'stm32cube\bundles\gnu-tools-for-stm32'
    $version = Get-ChildItem -LiteralPath $bundle -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $version) { throw 'STM32Cube GCC not found; pass -Compiler.' }
    $Compiler = Join-Path $version.FullName 'bin\arm-none-eabi-gcc.exe'
}
& $runtime -B -X utf8 (Join-Path $PSScriptRoot 'build.py') --compiler $Compiler
exit $LASTEXITCODE
