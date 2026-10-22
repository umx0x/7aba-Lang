$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'lists_files.7ba'
$executable = Join-Path $env:TEMP '7aba-lists-files-test.exe'
$tempRoot = Join-Path $env:TEMP ('7aba-lists-files-' + [guid]::NewGuid().ToString('N'))
$testDir = Join-Path $tempRoot 'tests'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

try {
    New-Item -ItemType Directory -Path $testDir -Force | Out-Null
    Copy-Item $fixture $testDir
    Push-Location $tempRoot
    try {
        & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
        if ($LASTEXITCODE -ne 0) {
            throw 'Strict GCC build failed.'
        }

        $copiedFixture = Join-Path $testDir 'lists_files.7ba'
        $firstOutput = (& $executable run $copiedFixture 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $firstOutput -notmatch 'LISTS_SETUP file I/O3') {
            throw "List/file setup fixture failed:`n$firstOutput"
        }

        $secondOutput = (& $executable run $copiedFixture 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) {
            throw "List/file verification fixture failed:`n$secondOutput"
        }

        foreach ($expected in @(
            'TABLE values (length=3)',
            '[0]',
            '-12',
            'PILE history (size=1, capacity=4)',
            '[0] value=9 <- SP (top)',
            'TABLE restored (length=3)',
            'VALUE_INDEX 14',
            'VALUE_LENGTH 3',
            'FILE "tests/data file.txt" (bytes=9, lines=1)',
            '[0] file I/O3'
        )) {
            if (-not $secondOutput.Contains($expected)) {
                throw "Expected output was missing: $expected`n$secondOutput"
            }
        }

        Write-Output 'PASS: managed list/pile mutation, expressions, length, persistence, and bounded file I/O'
    }
    finally {
        Pop-Location
    }
}
finally {
    Remove-Item -LiteralPath $executable, $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
