$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'exceptions.7ba'
$stateFile = "$fixture.state"
$executable = Join-Path $env:TEMP '7aba-exceptions-test.exe'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GCC build failed.'
    }

    $output = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Exception test execution failed:`n$output"
    }
    foreach ($expected in @('1', '17', '9', 'EXCEPTIONS_PASS')) {
        if (-not $output.Contains($expected)) {
            throw "Expected exception output was missing: $expected`n$output"
        }
    }

    Write-Output 'PASS: integer throw/catch, division-by-zero catch, and action unwinding'
}
finally {
    Remove-Item $stateFile, $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}
