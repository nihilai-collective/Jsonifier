param(
    [Parameter(Mandatory = $false)]
    [string]$FolderPath = ".",

    [Parameter(Mandatory = $false)]
    [string]$OutputFile = "concatenated_output.txt",

    [Parameter(Mandatory = $false)]
    [switch]$Recurse
)

if (-not (Test-Path -Path $FolderPath)) {
    Write-Error "Folder path '$FolderPath' does not exist."
    exit 1
}

if (Test-Path -Path $OutputFile) {
    Remove-Item -Path $OutputFile -Force
}

$getChildItemParams = @{
    Path   = $FolderPath
    Filter = "*.hpp"
}

if ($Recurse) {
    $getChildItemParams["Recurse"] = $true
}

$hppFiles = Get-ChildItem @getChildItemParams

$matchedCount = 0

foreach ($file in $hppFiles) {
    $content = Get-Content -Path $file.FullName -Raw

    if ($content -match "runTests") {
        Add-Content -Path $OutputFile -Value "// ===== $($file.FullName) ====="
        Add-Content -Path $OutputFile -Value $content
        Add-Content -Path $OutputFile -Value ""
        $matchedCount++
    }
}

Write-Host "Done. $matchedCount file(s) containing 'runTests' written to $OutputFile"