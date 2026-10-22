$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'oop.7ba'
$managedFixture = Join-Path $PSScriptRoot 'oop_managed.7ba'
$stateFiles = @("$fixture.state", "$managedFixture.state")
$executable = Join-Path $env:TEMP '7aba-oop-test.exe'
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
        throw "OOP test execution failed:`n$output"
    }

    foreach ($expected in @(
        '=== 7ABA LIST SORTING EXECUTION ===',
        'TABLE original (length=4)',
        '[0]',
        '34',
        'TABLE sortedList (length=4)',
        '5',
        '=== Sorting execution completed ==='
    )) {
        if (-not $output.Contains($expected)) {
            throw "Expected OOP output was missing: $expected`n$output"
        }
    }

    $managedOutput = (& $executable run $managedFixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Managed OOP fixture failed:`n$managedOutput"
    }
    foreach ($expected in @(
        'HERO Ada 100 rookie',
        'VICTIM_HP 55',
        'ALIAS_HP 103',
        'OBJECT hero (class=Hero, address=',
        'name = "Ada"',
        'hp = 103',
        'title = "captain"',
        'companion = [object]'
    )) {
        if (-not $managedOutput.Contains($expected)) {
            throw "Expected managed OOP output was missing: $expected`n$managedOutput"
        }
    }

    Write-Output 'PASS: class methods, managed objects, inheritance, overrides, aliases, fields, and display'
}
finally {
    Remove-Item -LiteralPath @($stateFiles + $executable) -Force -ErrorAction SilentlyContinue
    Pop-Location
}
