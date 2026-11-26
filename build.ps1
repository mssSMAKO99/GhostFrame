# GhostFrame build script
#
# Usage:
#   .\build.ps1                     # Release, launcher + test binaries, dist\GhostFrame.exe
#   .\build.ps1 -Configuration Debug
#   .\build.ps1 -SkipTests          # only the product, no test binaries
#   .\build.ps1 -Test               # build, then run the harnesses and the self-test
#   .\build.ps1 -Clean              # remove build\ and dist\ and exit
#
# The launcher project builds the x64/x86 agent and the 32-bit helper and embeds
# them as resources, so this is the only build entry point you need.

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$SkipTests,

    # Run the harnesses (GfAgentHost, GfSettingsHost) and, when a GfTestTarget
    # process is running, the end-to-end self-test.  Implies the test binaries.
    [switch]$Test,

    # Remove build\ and dist\ and exit.
    [switch]$Clean
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

# The product version lives in one place; parse it instead of duplicating it.
function Get-GfVersion {
    $header = Join-Path $root 'include\ghostframe\version.h'
    $text = Get-Content -LiteralPath $header -Raw
    $parts = foreach ($field in 'MAJOR', 'MINOR', 'PATCH', 'BUILD') {
        $match = [regex]::Match($text, "GF_VERSION_$field\s+(\d+)")
        if (-not $match.Success) {
            throw "GF_VERSION_$field not found in $header"
        }
        $match.Groups[1].Value
    }
    return ($parts -join '.')
}

function Invoke-Build([string]$project, [string]$platform) {
    & $script:msbuild $project /nologo /m /v:minimal `
        "/p:Configuration=$Configuration" "/p:Platform=$platform" "/p:SolutionDir=$root\"
    if ($LASTEXITCODE -ne 0) {
        throw "build failed: $project ($platform)"
    }
}

function Invoke-Harness([string]$exe, [string[]]$arguments) {
    Write-Host "Running $([System.IO.Path]::GetFileName($exe))"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "harness failed ($LASTEXITCODE): $exe"
    }
}

if ($Clean) {
    foreach ($dir in @((Join-Path $root 'build'), (Join-Path $root 'dist'))) {
        if (Test-Path $dir) {
            Write-Host "Removing $dir"
            Remove-Item -Recurse -Force $dir
        }
    }
    Write-Host 'Clean complete.'
    return
}

$script:msbuild = Find-MSBuild
$version = Get-GfVersion
Write-Host "MSBuild: $script:msbuild"
Write-Host "Configuration: $Configuration"
Write-Host "Version: $version"

$buildTests = (-not $SkipTests) -or $Test

Invoke-Build (Join-Path $root 'src\launcher\GhostFrame.Launcher.vcxproj') 'x64'
Invoke-Build (Join-Path $root 'src\brand\GfBrand.vcxproj') 'x64'

if ($buildTests) {
    Invoke-Build (Join-Path $root 'tests\target\GfTestTarget.vcxproj') 'x64'
    Invoke-Build (Join-Path $root 'tests\agent_host\GfAgentHost.vcxproj') 'x64'
    Invoke-Build (Join-Path $root 'tests\settings_host\GfSettingsHost.vcxproj') 'x64'
    Invoke-Build (Join-Path $root 'tests\brand_host\GfBrandHost.vcxproj') 'x64'
}

$output = Join-Path $root "build\out\x64\$Configuration"
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$product = Join-Path $output 'GhostFrame.exe'
$canonical = Join-Path $dist 'GhostFrame.exe'
Copy-Item $product $canonical -Force
Copy-Item $product (Join-Path $dist "GhostFrame-$version.exe") -Force

$hash = (Get-FileHash $canonical -Algorithm SHA256).Hash
# sha256sum-compatible line, so `sha256sum -c` and install.ps1 can both read it.
Set-Content -LiteralPath (Join-Path $dist 'GhostFrame.exe.sha256') `
    -Value "$hash  GhostFrame.exe" -Encoding Ascii

# GfBrand is a separate helper so installer branding never needs a source tree.
$brand = Join-Path $output 'GfBrand.exe'
$brandDist = Join-Path $dist 'GfBrand.exe'
Copy-Item $brand $brandDist -Force
$brandHash = (Get-FileHash $brandDist -Algorithm SHA256).Hash
Set-Content -LiteralPath (Join-Path $dist 'GfBrand.exe.sha256') `
    -Value "$brandHash  GfBrand.exe" -Encoding Ascii

# A zip keeps the release artifact intact in transit and is what CI uploads.
$zip = Join-Path $dist "GhostFrame-$version.zip"
$zipItems = @($canonical, (Join-Path $dist 'GhostFrame.exe.sha256'),
              $brandDist, (Join-Path $dist 'GfBrand.exe.sha256'))
foreach ($extra in 'install.ps1', 'README.md', 'LICENSE') {
    $candidate = Join-Path $root $extra
    if (Test-Path $candidate) {
        $zipItems += $candidate
    }
}
Compress-Archive -Path $zipItems -DestinationPath $zip -Force

$testOutput = Join-Path $root "build\out\x64\$Configuration"
if ($buildTests) {
    Write-Host ''
    Write-Host 'Test binaries:'
    Write-Host "  $testOutput\GfTestTarget.exe"
    Write-Host "  $testOutput\GfAgentHost.exe"
    Write-Host "  $testOutput\GfSettingsHost.exe"
    Write-Host "  $testOutput\GfBrandHost.exe"
}

if ($Test) {
    $agentDll = Join-Path $root "build\payload\$Configuration\GhostFrameAgent64.dll"
    Invoke-Harness (Join-Path $testOutput 'GfAgentHost.exe') @($agentDll)
    Invoke-Harness (Join-Path $testOutput 'GfSettingsHost.exe') @()
    Invoke-Harness (Join-Path $testOutput 'GfBrandHost.exe') @()

    $target = Get-Process -Name 'GfTestTarget' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($target) {
        Write-Host 'GfTestTarget is running; running the self-test.'
        & $product '--selftest'
        switch ($LASTEXITCODE) {
            0 { Write-Host 'Self-test passed.' }
            2 { Write-Host 'Self-test skipped (the target was not reachable).' }
            default { throw "self-test failed ($LASTEXITCODE)" }
        }
    } else {
        Write-Host 'GfTestTarget is not running; skipping the self-test.'
    }
}

Write-Host ''
Write-Host 'Artifact:  dist\GhostFrame.exe'
Write-Host "Versioned: dist\GhostFrame-$version.exe"
Write-Host 'Branding:  dist\GfBrand.exe'
Write-Host "Archive:   dist\GhostFrame-$version.zip"
Write-Host "SHA256:    $hash"
Write-Host "Brand SHA: $brandHash"
