$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $env:TEMP '7aba-diagnostic-span-test.exe'
$stdout = Join-Path $env:TEMP '7aba-diagnostic-span.out'
$stderr = Join-Path $env:TEMP '7aba-diagnostic-span.err'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c',
    'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c',
    'typecheck.c')

Push-Location $repositoryRoot
try {
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable $sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict diagnostic-span test build failed.'
    }

    $process = Start-Process -FilePath $executable `
        -ArgumentList "run `"$PSScriptRoot\diagnostic_span.7ba`"" `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
        -Wait -PassThru -NoNewWindow
    $diagnostic = [System.IO.File]::ReadAllText($stderr)
    if ($process.ExitCode -eq 0 -or
        $diagnostic -notmatch 'diagnostic_span\.7ba:1:18: runtime error\[E2001\]' -or
        $diagnostic -notmatch '(?m)\|\s+\^~\r?$') {
        throw "Expected the two-character <= operator to be underlined:`n$diagnostic"
    }
    Write-Output 'PASS: runtime comparison diagnostics underline the complete operator span'
}
finally {
    Remove-Item $executable, $stdout, $stderr -Force -ErrorAction SilentlyContinue
    Pop-Location
}
