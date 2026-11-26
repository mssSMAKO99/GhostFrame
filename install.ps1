# GhostFrame one-line installer.
#
#   irm overcumatosis.github.io/gf | iex
#
# The same file is also served from the project site:
#   irm https://overcumatosis.github.io/GhostFrame/install.ps1 | iex
#
# It downloads a release build from GitHub, verifies its SHA-256, optionally
# rebrands it with GfBrand.exe, installs it under %LOCALAPPDATA%\GhostFrame\bin,
# adds a Start Menu shortcut and starts it.
#
# This file is kept ASCII-only and without a BOM on purpose: GitHub Pages serves
# .ps1 as application/octet-stream, and Windows PowerShell 5.1 then decodes the
# body as ANSI. A BOM or any non-ASCII byte would break `irm | iex`.
#
# Parameters:
#   -Version      'latest' (default) or an exact release tag such as 3.0.0.0
#   -InstallDir   where the branded executable goes
#                 (default %LOCALAPPDATA%\GhostFrame\bin)
#   -BaseUrl      override the release base URL (forks and tests)
#   -Name         custom product name / executable file stem
#   -Description  custom FileDescription shown in Explorer Properties
#   -Company      custom CompanyName
#   -Emoji        use one emoji as the product icon
#   -Icon         use a supplied .ico instead of the bundled icon
#   -NoRun        install but do not start it
#   -Uninstall    stop GhostFrame, remove the branded install and all its data
#
# Omitted branding parameters are preserved from the previous install, so an
# update keeps the existing name, description, company and icon.

[CmdletBinding()]
param(
    [string]$Version = 'latest',

    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'GhostFrame\bin'),

    [string]$BaseUrl = 'https://github.com/overcumatosis/GhostFrame/releases',

    [string]$Name,

    [string]$Description,

    [string]$Company,

    [string]$Emoji,

    [string]$Icon,

    [switch]$NoRun,

    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# Windows 10 builds older than 2004 still default to TLS 1.0.
try {
    $tls = [Net.ServicePointManager]::SecurityProtocol
    [Net.ServicePointManager]::SecurityProtocol = $tls -bor [Net.SecurityProtocolType]::Tls12
} catch {
    # Older .NET without Tls12 in the enum: leave the default alone.
}

$dataDir = Join-Path $env:LOCALAPPDATA 'GhostFrame'
$manifestPath = Join-Path $dataDir 'branding.json'
$shortcutsPath = Join-Path $dataDir 'shortcuts.json'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# Resolve-Branding runs in its own scope, so remember which script parameters
# the caller actually supplied.
$script:nameBound = $PSBoundParameters.ContainsKey('Name')
$script:descriptionBound = $PSBoundParameters.ContainsKey('Description')
$script:companyBound = $PSBoundParameters.ContainsKey('Company')
$script:emojiBound = $PSBoundParameters.ContainsKey('Emoji')
$script:iconBound = $PSBoundParameters.ContainsKey('Icon')

function Get-AssetUrl {
    param([string]$Asset)

    if ($Version -eq 'latest') {
        return "$BaseUrl/latest/download/$Asset"
    }

    $tag = $Version
    if ($tag -notmatch '^v?\d') {
        $tag = "v$tag"
    }
    return "$BaseUrl/download/$tag/$Asset"
}

function Get-ShortcutPath {
    param([string]$Stem)

    return (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$Stem.lnk")
}

function ConvertTo-Stem {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return 'GhostFrame'
    }
    $invalid = [IO.Path]::GetInvalidFileNameChars()
    $builder = New-Object System.Text.StringBuilder
    foreach ($ch in $Value.Trim().ToCharArray()) {
        if ($invalid -contains $ch -or [int]$ch -lt 32) {
            [void]$builder.Append('_')
        } else {
            [void]$builder.Append($ch)
        }
    }
    $stem = $builder.ToString().TrimEnd(' ', '.')
    if ([string]::IsNullOrWhiteSpace($stem)) {
        return 'GhostFrame'
    }
    return $stem
}

function Get-Download {
    param([string]$Url, [string]$Destination)

    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
}

function Get-FileStem {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return 'GhostFrame'
    }
    return [IO.Path]::GetFileNameWithoutExtension($Path)
}

function Read-BrandingManifest {
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        return $null
    }
    try {
        return (Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)
    } catch {
        Write-Warning "Ignoring an unreadable branding manifest: $($_.Exception.Message)"
        return $null
    }
}

function Get-RunValue {
    param([string]$ValueName)

    if ([string]::IsNullOrWhiteSpace($ValueName)) {
        return $null
    }
    try {
        return (Get-ItemProperty -Path $runKey -Name $ValueName -ErrorAction Stop).$ValueName
    } catch {
        return $null
    }
}

function Remove-RunValue {
    param([string]$ValueName)

    if ([string]::IsNullOrWhiteSpace($ValueName)) {
        return
    }
    Remove-ItemProperty -Path $runKey -Name $ValueName -ErrorAction SilentlyContinue
}

function Stop-ProcessStem {
    param([string]$Stem)

    if ([string]::IsNullOrWhiteSpace($Stem)) {
        return
    }
    # Compare the process name exactly: -Name treats [, ] and * as wildcards.
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -eq $Stem } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Stop-GhostFrame {
    param([string[]]$Stems)

    $seen = @{}
    foreach ($stem in $Stems) {
        if ([string]::IsNullOrWhiteSpace($stem) -or $seen.ContainsKey($stem)) {
            continue
        }
        $seen[$stem] = $true
        Stop-ProcessStem $stem
    }
    # Give the processes a moment to release the executable and its payloads.
    Start-Sleep -Milliseconds 600
}

function Remove-RecordedShortcuts {
    param([string]$Exe)

    # Prefer the launcher itself: it knows the manifest format and clears it.
    if (-not [string]::IsNullOrWhiteSpace($Exe) -and (Test-Path -LiteralPath $Exe)) {
        try {
            $process = Start-Process -FilePath $Exe -ArgumentList '--remove-shortcuts' `
                -Wait -PassThru -WindowStyle Hidden
            if ($process.ExitCode -eq 0) {
                return
            }
        } catch {
            Write-Warning "Could not run the shortcut cleanup helper: $($_.Exception.Message)"
        }
    }

    # Fallback for a missing executable: delete the files listed in the
    # manifest directly.
    if (-not (Test-Path -LiteralPath $shortcutsPath)) {
        return
    }
    try {
        $document = Get-Content -LiteralPath $shortcutsPath -Raw | ConvertFrom-Json
        foreach ($entry in @($document.shortcuts)) {
            if ($entry -and -not [string]::IsNullOrWhiteSpace($entry.path)) {
                Remove-Item -LiteralPath $entry.path -Force -ErrorAction SilentlyContinue
            }
        }
    } catch {
        Write-Warning "Could not clean the shortcut manifest: $($_.Exception.Message)"
    }
}

function Resolve-Branding {
    param($Existing)

    $nameBound = $script:nameBound
    $descriptionBound = $script:descriptionBound
    $companyBound = $script:companyBound
    $emojiBound = $script:emojiBound
    $iconBound = $script:iconBound

    $brandName = if ($nameBound) { $Name } elseif ($Existing -and $Existing.name) { $Existing.name } else { 'GhostFrame' }
    if ([string]::IsNullOrWhiteSpace($brandName)) {
        $brandName = 'GhostFrame'
    }

    $brandDescription = if ($descriptionBound) { $Description } elseif ($Existing) { $Existing.description } else { '' }
    if (-not $nameBound -and $Existing -and [string]::IsNullOrWhiteSpace($brandDescription)) {
        $brandDescription = ''
    }
    # Do not carry a description that names the old product when the name changes.
    if ($nameBound -and $Existing -and -not $descriptionBound -and
        $Existing.name -and $Existing.name -ne $brandName) {
        $brandDescription = ''
    }

    $brandCompany = if ($companyBound) { $Company } elseif ($Existing) { $Existing.company } else { '' }

    $iconKind = 'bundled'
    $iconValue = ''
    if ($emojiBound -and $iconBound) {
        throw 'Choose either -Emoji or -Icon, not both.'
    }
    if ($iconBound) {
        if ([string]::IsNullOrWhiteSpace($Icon)) {
            throw '-Icon needs a path to an .ico file.'
        }
        $resolved = (Resolve-Path -LiteralPath $Icon -ErrorAction Stop).Path
        $iconKind = 'file'
        $iconValue = $resolved
    } elseif ($emojiBound) {
        if ([string]::IsNullOrWhiteSpace($Emoji)) {
            $iconKind = 'bundled'
        } else {
            $iconKind = 'emoji'
            $iconValue = $Emoji
        }
    } elseif ($Existing -and $Existing.iconKind) {
        $iconKind = $Existing.iconKind
        $iconValue = if ($Existing.iconSource) { $Existing.iconSource } else { '' }
        if ($iconKind -eq 'file' -and $iconValue -and -not (Test-Path -LiteralPath $iconValue)) {
            Write-Warning "The saved branding icon is missing; using the bundled icon."
            $iconKind = 'bundled'
            $iconValue = ''
        }
    }

    return [pscustomobject]@{
        Name        = $brandName
        Stem        = (ConvertTo-Stem $brandName)
        Description = $brandDescription
        Company     = $brandCompany
        IconKind    = $iconKind
        IconSource  = $iconValue
    }
}

function Install-GhostFrame {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

    $existing = Read-BrandingManifest
    $branding = Resolve-Branding $existing

    $target = Join-Path $InstallDir ($branding.Stem + '.exe')
    $shortcut = Get-ShortcutPath $branding.Stem

    $oldExe = if ($existing -and $existing.installedExePath) {
        $existing.installedExePath
    } else {
        Join-Path $InstallDir 'GhostFrame.exe'
    }
    $oldStem = Get-FileStem $oldExe
    $oldShortcut = if ($existing -and $existing.shortcutPath) {
        $existing.shortcutPath
    } else {
        Get-ShortcutPath $oldStem
    }
    $oldAutostart = if ($existing -and $existing.autostartValue) {
        $existing.autostartValue
    } else {
        $oldStem
    }

    # Preserve an enabled autostart entry across the rename.
    $hadAutostart = ($null -ne (Get-RunValue $oldAutostart)) -or
                    ($null -ne (Get-RunValue $oldStem)) -or
                    ($null -ne (Get-RunValue 'GhostFrame'))

    Stop-GhostFrame @($oldStem, $branding.Stem, 'GhostFrame')

    if (Test-Path -LiteralPath $oldShortcut) {
        Remove-Item -LiteralPath $oldShortcut -Force -ErrorAction SilentlyContinue
    }
    if ($oldExe -and $oldExe -ne $target -and (Test-Path -LiteralPath $oldExe)) {
        Remove-Item -LiteralPath $oldExe -Force -ErrorAction SilentlyContinue
    }
    Remove-RunValue $oldAutostart
    Remove-RunValue $oldStem
    Remove-RunValue 'GhostFrame'

    $tmp = Join-Path $env:TEMP ('GhostFrame-install-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    $exeTmp = Join-Path $tmp 'GhostFrame.exe'
    $hashTmp = Join-Path $tmp 'GhostFrame.exe.sha256'
    $brandTmp = Join-Path $tmp 'GfBrand.exe'
    $brandHashTmp = Join-Path $tmp 'GfBrand.exe.sha256'

    try {
        Get-Download (Get-AssetUrl 'GhostFrame.exe') $exeTmp
        Get-Download (Get-AssetUrl 'GhostFrame.exe.sha256') $hashTmp

        $expected = (Get-Content -LiteralPath $hashTmp -Raw).Trim().Split()[0].ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $exeTmp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            throw "SHA256 mismatch: expected $expected, got $actual"
        }
        Write-Host "SHA256 verified: $actual"

        $signature = Get-AuthenticodeSignature -LiteralPath $exeTmp
        if ($signature.Status -ne 'Valid') {
            Write-Warning "GhostFrame.exe is not digitally signed ($($signature.Status)). Compare the SHA256 above with the release page before running it."
        }

        $brandAvailable = $true
        try {
            Get-Download (Get-AssetUrl 'GfBrand.exe') $brandTmp
        } catch {
            $brandAvailable = $false
            Write-Warning "This release does not publish GfBrand.exe."
        }

        if ($brandAvailable) {
            try {
                Get-Download (Get-AssetUrl 'GfBrand.exe.sha256') $brandHashTmp
                $brandExpected = (Get-Content -LiteralPath $brandHashTmp -Raw).Trim().Split()[0].ToLowerInvariant()
                $brandActual = (Get-FileHash -LiteralPath $brandTmp -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($brandExpected -ne $brandActual) {
                    throw "GfBrand.exe SHA256 mismatch: expected $brandExpected, got $brandActual"
                }
                Write-Host "GfBrand SHA256 verified: $brandActual"
            } catch {
                if (Test-Path -LiteralPath $brandHashTmp) {
                    throw
                }
                Write-Warning "GfBrand.exe has no published hash; continuing with the downloaded helper."
            }
        }

        $customBranding = ($branding.Stem -ne 'GhostFrame') -or
                          (-not [string]::IsNullOrWhiteSpace($branding.Description)) -or
                          (-not [string]::IsNullOrWhiteSpace($branding.Company)) -or
                          ($branding.IconKind -ne 'bundled')

        if ($brandAvailable) {
            $brandArgs = @(
                '--input', $exeTmp,
                '--output', $target,
                '--name', $branding.Name,
                '--manifest', $manifestPath,
                '--install-dir', $InstallDir,
                '--shortcut', $shortcut,
                '--autostart', $branding.Stem
            )
            if (-not [string]::IsNullOrWhiteSpace($branding.Description)) {
                $brandArgs += @('--description', $branding.Description)
            }
            if (-not [string]::IsNullOrWhiteSpace($branding.Company)) {
                $brandArgs += @('--company', $branding.Company)
            }
            if ($branding.IconKind -eq 'emoji' -and -not [string]::IsNullOrWhiteSpace($branding.IconSource)) {
                $brandArgs += @('--emoji', $branding.IconSource)
            } elseif ($branding.IconKind -eq 'file' -and -not [string]::IsNullOrWhiteSpace($branding.IconSource)) {
                $brandArgs += @('--icon', $branding.IconSource)
            }

            & $brandTmp @brandArgs
            if ($LASTEXITCODE -ne 0) {
                throw "GfBrand.exe failed with exit code $LASTEXITCODE"
            }
        } elseif ($customBranding) {
            throw 'Custom branding needs a release that includes GfBrand.exe. Install a newer release or omit -Name/-Description/-Company/-Emoji/-Icon.'
        } else {
            Copy-Item -LiteralPath $exeTmp -Destination $target -Force
        }

        # Branding changes the hash, so say so explicitly after the verified base.
        $installedHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($installedHash -ne $actual) {
            Write-Host "Installed hash (after branding): $installedHash"
        }

        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $shortcut) | Out-Null
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut($shortcut)
        $link.TargetPath = $target
        $link.WorkingDirectory = $InstallDir
        $link.Description = if (-not [string]::IsNullOrWhiteSpace($branding.Description)) {
            $branding.Description
        } else {
            $branding.Name
        }
        $link.IconLocation = "$target,0"
        $link.Save()

        if ($hadAutostart) {
            Set-ItemProperty -Path $runKey -Name $branding.Stem `
                -Value ('"' + $target + '" --daemon')
        }

        Write-Host "Installed: $target"
        Write-Host "Shortcut:  $shortcut"
        Write-Host "Branding:  $($branding.Name) ($($branding.IconKind) icon)"
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    if (-not $NoRun) {
        Start-Process -FilePath $target
    }
    Write-Host 'Done.'
}

function Uninstall-GhostFrame {
    $existing = Read-BrandingManifest
    $installedExe = if ($existing -and $existing.installedExePath) {
        $existing.installedExePath
    } else {
        Join-Path $InstallDir 'GhostFrame.exe'
    }
    $stems = @('GhostFrame', (Get-FileStem $installedExe))
    if ($existing -and $existing.name) {
        $stems += (ConvertTo-Stem $existing.name)
    }
    if ($existing -and $existing.autostartValue) {
        $stems += $existing.autostartValue
    }

    Stop-GhostFrame $stems

    Remove-RecordedShortcuts $installedExe

    $runNames = @('GhostFrame') + $stems
    if ($existing -and $existing.autostartValue) {
        $runNames += $existing.autostartValue
    }
    foreach ($runName in ($runNames | Select-Object -Unique)) {
        Remove-RunValue $runName
    }

    $links = @()
    if ($existing -and $existing.shortcutPath) {
        $links += $existing.shortcutPath
    }
    $links += (Get-ShortcutPath 'GhostFrame')
    foreach ($stem in ($stems | Select-Object -Unique)) {
        $links += (Get-ShortcutPath $stem)
    }
    foreach ($linkPath in ($links | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $linkPath) {
            Write-Host "Removing $linkPath"
            Remove-Item -LiteralPath $linkPath -Force -ErrorAction SilentlyContinue
        }
    }

    $installDirs = @($InstallDir)
    if ($existing -and $existing.installDir) {
        $installDirs += $existing.installDir
    }
    foreach ($path in ($installDirs | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $path) {
            Write-Host "Removing $path"
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if (Test-Path -LiteralPath $dataDir) {
        Write-Host "Removing $dataDir"
        Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host 'GhostFrame uninstalled.'
}

if ($Uninstall) {
    Uninstall-GhostFrame
} else {
    Install-GhostFrame
}