$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$tempRoot = Join-Path $env:TEMP ('7aba-input-multiple-' + [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $tempRoot 'input_multiple.7ba'
$stateFile = "$fixture.state"
$executable = Join-Path $env:TEMP '7aba-input-multiple-test.exe'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

function Invoke-InputFixture([string]$inputText, [bool]$sendInput) {
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $executable
    $process.StartInfo.Arguments = 'run "' + $fixture + '"'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardInput = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    if (-not $process.Start()) {
        throw 'Unable to start the input fixture process.'
    }
    if ($sendInput) {
        $process.StandardInput.WriteLine($inputText)
    }
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $result = [PSCustomObject]@{
        ExitCode = $process.ExitCode
        Output = $stdout + $stderr
    }
    $process.Dispose()
    return $result
}

New-Item -ItemType Directory -Path $tempRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'input_multiple.7ba') -Destination $fixture
try {
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GCC build failed.'
    }

    $validRun = Invoke-InputFixture '"Ada Lovelace" 36 London 7' $true
    $output = $validRun.Output
    if ($validRun.ExitCode -ne 0) {
        throw "Multiple input run failed:`n$output"
    }

    foreach ($expected in @('Person: Ada Lovelace', 'Age: 36', 'City: London', 'Score: 7')) {
        if (-not $output.Contains($expected)) {
            throw "Expected output was missing: $expected`n$output"
        }
    }

    $invalidInputs = @(
        @{ Input = '36'; Message = 'expected 4 value(s), received 1' },
        @{ Input = '"unterminated 36 London 7'; Message = 'quoted strings shorter than 256 bytes' },
        @{ Input = ('"' + ('x' * 300) + '" 36 London 7'); Message = 'quoted strings shorter than 256 bytes' },
        @{ Input = ('x' * 1024); Message = '1023-character limit' }
    )
    foreach ($invalidInput in $invalidInputs) {
        $failure = Invoke-InputFixture $invalidInput.Input $true
        if ($failure.ExitCode -eq 0 -or -not $failure.Output.Contains($invalidInput.Message)) {
            throw "Expected input validation failure was not reported:`n$($failure.Output)"
        }
    }
    $eofRun = Invoke-InputFixture '' $false
    if ($eofRun.ExitCode -eq 0 -or
        -not $eofRun.Output.Contains('Unable to read an input line')) {
        throw "Expected end-of-file input error was not reported:`n$($eofRun.Output)"
    }

    Write-Output 'PASS: mixed-type input and invalid input diagnostics'
}
finally {
    Remove-Item -LiteralPath @($stateFile, $executable) -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}