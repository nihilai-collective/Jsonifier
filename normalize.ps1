<#
.SYNOPSIS
    Recursively normalizes all line endings to LF (\n) inside a ./vcpkg folder.

.DESCRIPTION
    Walks every file under .\vcpkg (excluding .git internals and binary files),
    converts any CRLF or lone CR line endings to LF, and rewrites the file
    in place without a trailing BOM.

    Run this from the parent folder that CONTAINS the vcpkg folder, e.g.:
        C:\users\chris\source\repos>  .\Normalize-VcpkgLineEndings.ps1
#>

param(
    [string]$TargetFolder = ".\vcpkg"
)

if (-not (Test-Path $TargetFolder)) {
    Write-Host "Could not find folder: $TargetFolder" -ForegroundColor Red
    exit 1
}

$resolvedTarget = (Resolve-Path $TargetFolder).Path
Write-Host "Normalizing line endings under: $resolvedTarget" -ForegroundColor Cyan

$excludedDirs = @(".git")

$binaryExtensions = @(
    ".exe", ".dll", ".so", ".dylib", ".a", ".lib", ".obj", ".o",
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".webp",
    ".zip", ".7z", ".tar", ".gz", ".bz2", ".xz",
    ".pdf", ".ttf", ".otf", ".woff", ".woff2",
    ".db", ".sqlite", ".bin", ".dat", ".pyc", ".class"
)

$files = Get-ChildItem -Path $resolvedTarget -Recurse -File -Force | Where-Object {
    $path = $_.FullName
    $isExcludedDir = $excludedDirs | Where-Object { $path -match [Regex]::Escape("\$_\") }
    $isBinary = $binaryExtensions -contains $_.Extension.ToLower()
    -not $isExcludedDir -and -not $isBinary
}

$totalFiles = $files.Count
$changedCount = 0
$skippedCount = 0
$current = 0

foreach ($file in $files) {
    $current++
    Write-Progress -Activity "Normalizing line endings" -Status "$current / $totalFiles" -PercentComplete (($current / [Math]::Max($totalFiles,1)) * 100)

    try {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)

        if ($bytes.Length -eq 0) {
            continue
        }

        $hasNullByte = $bytes -contains 0
        if ($hasNullByte) {
            $skippedCount++
            continue
        }

        $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
        $encoding = New-Object System.Text.UTF8Encoding($false)

        $text = if ($hasBom) {
            $encoding.GetString($bytes, 3, $bytes.Length - 3)
        } else {
            $encoding.GetString($bytes)
        }

        $normalized = $text -replace "`r`n", "`n"
        $normalized = $normalized -replace "`r", "`n"

        if ($normalized -ne $text -or $hasBom) {
            [System.IO.File]::WriteAllText($file.FullName, $normalized, $encoding)
            $changedCount++
        }
    }
    catch {
        Write-Host "Skipped (read error): $($file.FullName) - $($_.Exception.Message)" -ForegroundColor DarkYellow
        $skippedCount++
    }
}

Write-Progress -Activity "Normalizing line endings" -Completed

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Files scanned:  $totalFiles"
Write-Host "  Files changed:  $changedCount"
Write-Host "  Files skipped:  $skippedCount (binary or unreadable)"
Write-Host ""
Write-Host "Review with 'git status' / 'git diff' before committing." -ForegroundColor Cyan