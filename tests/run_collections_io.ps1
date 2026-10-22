$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'collections_io.7ba'
$stateFile = "$fixture.state"
$dataDirectory = Join-Path $PSScriptRoot 'collections'
$dataFile = Join-Path $dataDirectory 'io.tmp'
$createdDataDirectory = -not (Test-Path -LiteralPath $dataDirectory)
$executable = Join-Path $env:TEMP '7aba-collections-io-test.exe'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    if ($createdDataDirectory) {
        New-Item -ItemType Directory -Path $dataDirectory | Out-Null
    }
    Remove-Item $stateFile, $dataFile -Force -ErrorAction SilentlyContinue
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GCC build failed.'
    }

    $firstOutput = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $firstOutput -notmatch 'SETUP_PASS 7ABA file I/O works') {
        throw "Feature setup run failed:`n$firstOutput"
    }

    $secondOutput = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Feature verification run failed:`n$secondOutput"
    }

    $expectedOutput = @(
        'MATRIX_PERSISTENCE_PASS',
        'QUEUE_FIFO_PASS',
        'FRONT -> [0] value=22',
        'BACK -> [2] value=44',
        'FILE_READ_PASS 7ABA file I/O works',
        'MATRIX_LENGTH_PASS',
        'QUEUE_LENGTH_PASS'
    )
    foreach ($expected in $expectedOutput) {
        if (-not $secondOutput.Contains($expected)) {
            throw "Expected output was missing: $expected`n$secondOutput"
        }
    }

    Write-Output 'PASS: matrix, FIFO queue, text file I/O, and persistence'
}
finally {
    Remove-Item $stateFile, $dataFile, $executable -Force -ErrorAction SilentlyContinue
    if ($createdDataDirectory) {
        Remove-Item $dataDirectory -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}