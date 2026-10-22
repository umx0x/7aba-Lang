$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$temporaryExecutables = New-Object System.Collections.Generic.List[string]

function Invoke-C99Test {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,
        [Parameter(Mandatory = $true)]
        [string[]] $Sources,
        [string[]] $LinkArgs = @()
    )

    $executable = Join-Path $env:TEMP "7aba-$Name-test.exe"
    $temporaryExecutables.Add($executable)
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @Sources @LinkArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Strict C99 build failed for $Name."
    }

    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw "Unit test failed: $Name."
    }
}

Push-Location $repositoryRoot
try {
    Invoke-C99Test -Name 'ast' -Sources @('tests/ast_test.c', 'ast.c')
    Invoke-C99Test -Name 'program-ast' -Sources @('program_ast.c', 'ast.c', 'tests/program_ast_test.c')
    Invoke-C99Test -Name 'resolver' -Sources @('resolver.c', 'program_ast.c', 'ast.c', 'tests/resolver_test.c')
    Invoke-C99Test -Name 'typecheck' -Sources @('tests/typecheck_test.c', 'typecheck.c', 'ast.c')

    $runtimeSources = @(
        'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c',
        'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c',
        'typecheck.c'
    )
    Invoke-C99Test -Name 'type-annotations' -Sources (@('tests/type_annotations_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'value' -Sources (@('tests/value_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'ast-bytecode' -Sources (@('tests/ast_bytecode_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'runtime-context' -Sources (@('tests/runtime_context_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'scope-gc' -Sources (@('tests/scope_gc_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'gc-safe-point' -Sources (@('tests/gc_safe_point_test.c') + $runtimeSources) -LinkArgs @('-lm')
    Invoke-C99Test -Name 'stdlib-ffi' -Sources (@('tests/stdlib_ffi_test.c') + $runtimeSources) -LinkArgs @('-lm')

    $integrationScripts = Get-ChildItem $PSScriptRoot -Filter 'run_*.ps1' |
        Where-Object { $_.Name -ne 'run_all.ps1' } |
        Sort-Object Name
    foreach ($script in $integrationScripts) {
        Write-Output "=== $($script.Name) ==="
        & powershell -NoProfile -ExecutionPolicy Bypass -File $script.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Integration test failed: $($script.Name)."
        }
    }

    Write-Output 'All 7aba C unit tests and integration scripts passed.'
}
finally {
    foreach ($executable in $temporaryExecutables) {
        Remove-Item -LiteralPath $executable -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}
