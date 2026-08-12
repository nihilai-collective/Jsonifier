Get-ChildItem -Path . -Recurse -Include *.hpp, *.cpp -File | Where-Object {
    $_.FullName -notmatch '\\(out|build)\\'
} | ForEach-Object {
    Write-Host "Formatting: $($_.FullName)"
    clang-format -i $_.FullName
}