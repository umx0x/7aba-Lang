param(
    [ValidateRange(1, 100000000)]
    [long]$Iterations = 1000000
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $env:TEMP '7aba-vm-benchmark.exe'
$sources = @('lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c',
    'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable `
        tests\vm_benchmark.c @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict VM benchmark build failed.'
    }
    & $executable $Iterations
    if ($LASTEXITCODE -ne 0) {
        throw 'VM benchmark execution failed.'
    }
}
finally {
    Remove-Item $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}
