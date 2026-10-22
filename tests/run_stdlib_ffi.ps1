$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$extension = if ($env:OS -eq 'Windows_NT') { '.dll' } else { '.so' }
$ffiLinkArgs = if ($env:OS -eq 'Windows_NT') { @() } else { @('-ldl') }
$library = Join-Path $env:TEMP "7aba-ffi-fixture$extension"
$executable = Join-Path $env:TEMP '7aba-stdlib-ffi-test.exe'

Push-Location $repositoryRoot
try {
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -shared -o $library tests\ffi_fixture.c
    if ($LASTEXITCODE -ne 0) {
        throw 'FFI fixture library build failed.'
    }
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable tests\stdlib_ffi_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c stdlib.c value.c gc.c runtime.c ffi.c ast.c typecheck.c @ffiLinkArgs -lm
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict StdLib/FFI test build failed.'
    }
    & $executable $library
    if ($LASTEXITCODE -ne 0) {
        throw 'StdLib/FFI tests failed.'
    }
    Write-Output 'PASS: stream I/O, array operations, and typed native calls'
}
finally {
    Remove-Item $library, $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}
