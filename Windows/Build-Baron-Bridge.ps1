# Copyright (C) 2026 Baron Potato
# SPDX-License-Identifier: GPL-3.0-only

[CmdletBinding()]
param(
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$SdkHeaders = '',
    [string]$ISCC = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Program) exited with code $LASTEXITCODE"
    }
}

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$source = Join-Path $PSScriptRoot 'BaronBridge'
$build = Join-Path $PSScriptRoot 'build-baron-bridge'
$payload = Join-Path $build ('payload-' + [Guid]::NewGuid().ToString('N'))
$dist = Join-Path $PSScriptRoot 'dist'
if (-not $SdkHeaders) {
    $SdkHeaders = Join-Path $source 'third_party\steinberg-sdk'
}
$configure = @('-S', $source, '-B', $build, '-G', $Generator, '-A', 'x64',
    "-DBARON_SDK_HEADERS=$SdkHeaders")
Invoke-Checked $cmake $configure
Invoke-Checked $cmake @('--build', $build, '--config', 'Release', '--parallel')
Invoke-Checked $cmake @('--install', $build, '--config', 'Release', '--prefix', $payload)

if (-not $ISCC) {
    $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compiler) { $ISCC = $compiler.Source }
    else {
        $ISCC = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
        if (-not (Test-Path -LiteralPath $ISCC -PathType Leaf)) {
            $ISCC = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
        }
    }
}
if (-not (Test-Path -LiteralPath $ISCC -PathType Leaf)) {
    throw 'Supply the path to the Inno Setup compiler with -ISCC.'
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Invoke-Checked $ISCC @("/DBridgePayload=$payload", "/O$dist", (Join-Path $source 'Installer.iss'))
Get-FileHash -LiteralPath (Join-Path $dist 'Baron-Bridge-x64-Setup.exe') -Algorithm SHA256
