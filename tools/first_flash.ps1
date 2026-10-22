param(
    [ValidateSet('build', 'deploy', 'flash', 'verify')][string]$Action = 'build',
    [string]$Version = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$PowerSimulation,
    [switch]$MatsFailTest,
    [string]$Bundle = '',
    [string]$Serial = '',
    [ValidateSet('NO', 'YES')][string]$ConfirmIdle = 'NO',
    [ValidateSet('NO', 'YES')][string]$ConfirmBlank = 'NO'
)
$ErrorActionPreference = 'Stop'
# V2专用入口固定板型；底层统一复用独立Boot工程的首烧工具，不改变日常OTA入口。
$appRoot = Split-Path -Parent $PSScriptRoot
$entry = Join-Path (Split-Path -Parent $appRoot) 'bootloader\tools\flash.ps1'
& $entry -Board apollo-v2 -AppRoot $appRoot -Action $Action -Version $Version -Configuration $Configuration `
    -PowerSimulation:$PowerSimulation -MatsFailTest:$MatsFailTest -Bundle $Bundle -Serial $Serial -ConfirmIdle $ConfirmIdle -ConfirmBlank $ConfirmBlank
exit $LASTEXITCODE
