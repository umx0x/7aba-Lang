$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $env:TEMP '7aba-gc-safe-point-test.exe'
$sources = @('lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c',
    'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable `
        tests\gc_safe_point_test.c @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GC safe-point test build failed.'
    }
    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw 'Automatic GC safe-point tests failed.'
    }
}
finally {
    Remove-Item $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}
