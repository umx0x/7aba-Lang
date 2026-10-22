$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'language_collections.7ba'
$invalidFixture = Join-Path $PSScriptRoot 'language_collection_invalid_key.7ba'
$missingFixture = Join-Path $PSScriptRoot 'language_collection_missing_key.7ba'
$persistenceFixture = Join-Path $PSScriptRoot 'language_scalar_persistence.7ba'
$stateFiles = @("$fixture.state", "$invalidFixture.state", "$missingFixture.state",
    "$persistenceFixture.state")
$executable = Join-Path $env:TEMP '7aba-language-collections-test.exe'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    Remove-Item $stateFiles -Force -ErrorAction SilentlyContinue
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GCC build failed.'
    }

    $output = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Language-level collection fixture failed:`n$output"
    }

    $expectedOutput = @(
        'MAP_INDEX_PASS Grace',
        'MAP_ALIAS_PASS seven',
        'SCALAR_STRING_COPY_PASS Grace',
        'SCALAR_ARITHMETIC_PASS 9',
        'NULL_KEY_PASS nothing',
        'BOOL_KEY_PASS inactive',
        'MAP_MEMBERSHIP_PASS',
        'SET_MEMBERSHIP_PASS',
        'DIRECT_MAP_MEMBERSHIP_PASS',
        'DIRECT_SET_MEMBERSHIP_PASS',
        'true',
        'MAP_LENGTH_PASS',
        'SET_DEDUP_LENGTH_PASS',
        'EMPTY_MAP_PASS',
        'EMPTY_SET_PASS'
    )
    foreach ($expected in $expectedOutput) {
        if (-not $output.Contains($expected)) {
            throw "Expected output was missing: $expected`n$output"
        }
    }

    $firstPersistenceOutput = (& $executable run $persistenceFixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or
        $firstPersistenceOutput -notmatch 'SCALAR_PERSISTENCE 1') {
        throw "First scalar persistence run failed:`n$firstPersistenceOutput"
    }
    $secondPersistenceOutput = (& $executable run $persistenceFixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or
        $secondPersistenceOutput -notmatch 'SCALAR_PERSISTENCE 2' -or
        $secondPersistenceOutput -notmatch 'SCALAR_STRING_PERSISTENCE persisted scalar string') {
        throw "Second scalar persistence run failed:`n$secondPersistenceOutput"
    }

    $expectedFailures = @(
        @{ Path = $invalidFixture; Message = 'invalid key' },
        @{ Path = $missingFixture; Message = 'does not exist' }
    )
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    foreach ($expectedFailure in $expectedFailures) {
        $failureOutput = (& $executable run $expectedFailure.Path 2>&1 | Out-String)
        $failureExitCode = $LASTEXITCODE
        if ($failureExitCode -eq 0 -or $failureOutput -notmatch $expectedFailure.Message) {
            $ErrorActionPreference = $savedErrorActionPreference
            throw "Expected collection runtime error was not reported:`n$failureOutput"
        }
    }
    $ErrorActionPreference = $savedErrorActionPreference

    Write-Output 'PASS: map/set literals, lookup, mutation, membership, aliasing, and errors'
}
finally {
    Remove-Item -Path @($stateFiles + $executable) -Force -ErrorAction SilentlyContinue
    Pop-Location
}
