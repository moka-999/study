param(
    [ValidateSet('build', 'flash', 'verify')][string]$Action = 'build',
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet(2, 4)][int]$RkUart = 4,
    [string]$Bundle = '',
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO',
    [ValidateSet('NO', 'YES')][string]$ConfirmBlank = 'NO'
)
$ErrorActionPreference = 'Stop'
# B1入口固定生产板型，不接收V2模拟参数；UART选择必须与实际RK接线一致。
$appRoot = Split-Path -Parent $PSScriptRoot
$entry = Join-Path (Split-Path -Parent $appRoot) 'bootloader\tools\flash.ps1'
& $entry -Board b1 -AppRoot $appRoot -Action $Action -Version $Version -Configuration $Configuration `
    -RkUart $RkUart -Bundle $Bundle -Serial $Serial -ConfirmIdle $ConfirmIdle -ConfirmBlank $ConfirmBlank
exit $LASTEXITCODE
