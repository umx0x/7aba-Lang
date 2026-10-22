$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'oop_constructor_args.7ba'
$stateFile = "$fixture.state"
$executable = Join-Path $env:TEMP '7aba-oop-constructor-args-test.exe'
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
        throw "OOP constructor/property test failed:`n$output"
    }
    foreach ($expected in @('Arthur', '150', '5', '300', 'Training Dummy', '55', 'Lancelot',
            'PROPERTY_STRING_COPY_PASS Lancelot')) {
        if (-not $output.Contains($expected)) {
            throw "Expected OOP output was missing: $expected`n$output"
        }
    }

    Write-Output 'PASS: string/numeric constructor arguments and external object property mutation'
}
finally {
    Remove-Item $stateFile, $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}
