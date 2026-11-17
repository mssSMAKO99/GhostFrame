# GhostFrame build script
#
# Usage:
#   .\build.ps1                     # Release, launcher + test binaries, dist\GhostFrame.exe
#   .\build.ps1 -Configuration Debug
#   .\build.ps1 -SkipTests          # only the product
#
# The launcher project builds the x64/x86 agent and the 32-bit helper and embeds
# them as resources, so this is the only build entry point you need.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    $patterns = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe')
    )
    foreach ($pattern in $patterns) {
        $candidate = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw 'MSBuild was not found. Install Visual Studio 2022 or newer with the "Desktop development with C++" workload.'
}

function Invoke-Build([string]$project, [string]$platform) {
    & $script:msbuild $project /nologo /m /v:minimal `
        "/p:Configuration=$Configuration" "/p:Platform=$platform" "/p:SolutionDir=$root\"
    if ($LASTEXITCODE -ne 0) {
        throw "build failed: $project ($platform)"
    }
}

$script:msbuild = Find-MSBuild
Write-Host "MSBuild: $script:msbuild"
Write-Host "Configuration: $Configuration"

Invoke-Build (Join-Path $root 'src\launcher\GhostFrame.Launcher.vcxproj') 'x64'

if (-not $SkipTests) {
    Invoke-Build (Join-Path $root 'tests\target\GfTestTarget.vcxproj') 'x64'
    Invoke-Build (Join-Path $root 'tests\agent_host\GfAgentHost.vcxproj') 'x64'
}

$output = Join-Path $root "build\out\x64\$Configuration"
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$product = Join-Path $output 'GhostFrame.exe'
Copy-Item $product (Join-Path $dist 'GhostFrame.exe') -Force

$hash = (Get-FileHash (Join-Path $dist 'GhostFrame.exe') -Algorithm SHA256).Hash
Write-Host ''
Write-Host 'Artifact: dist\GhostFrame.exe'
Write-Host "SHA256:   $hash"
