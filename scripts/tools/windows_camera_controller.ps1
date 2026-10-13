# Copyright (c) 2026 Project CHIP Authors
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64",
    [string] $OutputRoot,
    [switch] $RunTests,
    [switch] $RunMediaSelfTest
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $RepoRoot "out\windows-camera-controller-$Architecture"
}
$OutputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
if (($RunTests -or $RunMediaSelfTest) -and $Architecture -ne "x64") {
    throw "Run ARM64 binaries on an ARM64 host; this script's execution checks require x64."
}
. (Join-Path $RepoRoot "scripts\setup\windows.ps1") -Architecture $Architecture
$project = Join-Path $RepoRoot "examples\camera-controller\windows\WebView2Packages.csproj"
$restoreRoot = Join-Path $OutputRoot "webview2-restore"
& dotnet restore $project --verbosity quiet "-p:MSBuildProjectExtensionsPath=$restoreRoot\"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$assets = Get-Content (Join-Path $restoreRoot "project.assets.json") -Raw | ConvertFrom-Json
$webview = $null
foreach ($folder in $assets.packageFolders.PSObject.Properties.Name) {
    $candidate = Join-Path $folder "microsoft.web.webview2\1.0.3719.77"
    if (Test-Path (Join-Path $candidate "build\native\include\WebView2.h") -PathType Leaf) {
        $webview = $candidate
        break
    }
}
if (-not $webview) {
    throw "The restored native WebView2 package was not found in the NuGet package folders."
}
$gnArguments = @(
    'target_os="win"'
    "target_cpu=`"$Architecture`""
    'chip_device_platform="windows"'
    'chip_windows_build_camera_controller=true'
    'chip_windows_enable_cxx20=true'
    'chip_with_nlfaultinjection=false'
    'chip_build_tests=false'
    'chip_build_tools=true'
    'chip_caller_handles_critical_failure=true'
    'config_use_interactive_mode=true'
    'config_enable_yaml_tests=false'
    'config_enable_https_requests=false'
    'chip_build_webrtc_signaling=true'
    'chip_support_webrtc_python_bindings=false'
    "chip_windows_webview2_root=`"$($webview.Replace('\', '/'))`""
) -join " "
& gn "--root=$RepoRoot" gen $OutputRoot "--args=$gnArguments"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$targets = @("chip-camera-controller", "camera-liveview-tests", "camera-liveview-flow-tests", "msvc-webrtc-signaling-tests")
& ninja -C $OutputRoot @targets
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item (Join-Path $webview "LICENSE.txt") (Join-Path $OutputRoot "WebView2-LICENSE.txt")
if ($RunTests) {
    & (Join-Path $OutputRoot "camera-liveview-tests.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $OutputRoot "msvc-webrtc-signaling-tests.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($RunMediaSelfTest) {
    & (Join-Path $OutputRoot "camera-liveview-flow-tests.exe")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $OutputRoot "chip-camera-controller.exe") --media-self-test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
