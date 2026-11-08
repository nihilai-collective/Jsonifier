# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# run-clang-format.ps1

Get-ChildItem -Path . -Recurse -Include *.hpp, *.cpp -File | Where-Object {
    $_.FullName -notmatch '\\(out|build)\\'
} | ForEach-Object {
    Write-Host "Formatting: $($_.FullName)"
    clang-format -i $_.FullName
}