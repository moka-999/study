param(
    [ValidateSet('build', 'deploy', 'flash', 'verify')][string]$Action = 'build',
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet(4)][int]$RkUart = 4,
    [string]$Bundle = '',
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO',
    [ValidateSet('NO', 'YES')][string]$ConfirmBlank = 'NO'
)
$ErrorActionPreference = 'Stop'
# B1入口固定生产板型及UART4协议，不接收V2模拟参数或旧USART2协议配置。
$appRoot = Split-Path -Parent $PSScriptRoot
$entry = Join-Path (Split-Path -Parent $appRoot) 'bootloader\tools\flash.ps1'
& $entry -Board b1 -AppRoot $appRoot -Action $Action -Version $Version -Configuration $Configuration `
    -RkUart $RkUart -Bundle $Bundle -Serial $Serial -ConfirmIdle $ConfirmIdle -ConfirmBlank $ConfirmBlank
exit $LASTEXITCODE
