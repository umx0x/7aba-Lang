$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot 'display_structures.7ba'
$stateFile = "$fixture.state"
$dataFile = Join-Path $PSScriptRoot 'display_file.txt'
$executable = Join-Path $env:TEMP '7aba-display-structures-test.exe'
$sources = @('main.c', 'lexer.c', 'parser.c', 'eval.c', 'bytecode.c', 'diagnostic.c', 'value.c', 'gc.c', 'runtime.c', 'stdlib.c', 'ffi.c', 'ast.c', 'typecheck.c') |
    ForEach-Object { Join-Path $repositoryRoot $_ }

Push-Location $repositoryRoot
try {
    Remove-Item $stateFile, $dataFile -Force -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($dataFile, "alpha`nbeta`n", [System.Text.Encoding]::ASCII)
    & gcc -Wall -Wextra -Werror -O3 -std=c99 -o $executable @sources
    if ($LASTEXITCODE -ne 0) {
        throw 'Strict GCC build failed.'
    }

    $setupOutput = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Display setup run failed:`n$setupOutput"
    }

    $output = (& $executable run $fixture 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Display verification run failed:`n$output"
    }

    foreach ($expected in @(
        'TABLE values (length=2)',
        '[0]',
        '-12',
        'TABLE empty_values (length=0)',
        '<empty>',
        'PILE empty_pile (size=0, capacity=4)',
        '<empty pile> (SP: none)',
        'MATRIX grid (2 rows x 3 columns)',
        '[i]\[j]',
        'QUEUE jobs (FIFO, size=2, capacity=8)',
        'FRONT -> [0] value=31',
        'BACK -> [1] value=42',
        'FULL: NO',
        'QUEUE empty_queue (FIFO, size=0, capacity=8)',
        'FILE "tests/display_file.txt" (bytes=11, lines=2)',
        '[0] alpha',
        '[1] beta'
    )) {
        if (-not $output.Contains($expected)) {
            throw "Expected display output was missing: $expected`n$output"
        }
    }
    if ($output -notmatch 'PILE history \(size=\d+, capacity=\d+\)') {
        throw "Persisted pile display heading was missing:`n$output"
    }
    if ($output -notmatch '\[\d+\] value=\d+ <- SP \(top\)') {
        throw "Pile top pointer was not shown on the top item:`n$output"
    }
    if ($output -match '(?m)^(SP:|FULL:|EMPTY:)') {
        throw "Pile/queue status was printed outside its box:`n$output"
    }
    $vertical = [char]0x2502
    $fullStatusRows = @($output -split "`r?`n" | Where-Object {
        $_.Contains('FULL: NO') -and $_[0] -eq $vertical -and $_[-1] -eq $vertical
    })
    if ($fullStatusRows.Count -eq 0) {
        throw "Queue full state was not shown inside its box:`n$output"
    }

    foreach ($borderCharacter in @(
        [char]0x250c,
        [char]0x2500,
        [char]0x2502,
        [char]0x2514
    )) {
        if (-not $output.Contains([string]$borderCharacter)) {
            throw "Expected Unicode box-drawing character was missing: U+{0:X4}`n{1}" -f [int]$borderCharacter, $output
        }
    }

    $lines = $output -split "`r?`n"
    foreach ($grid in @(
        @{ Heading = 'TABLE values (length=2)'; DataRows = 2 },
        @{ Heading = 'TABLE empty_values (length=0)'; DataRows = 1 },
        @{ Heading = 'MATRIX grid (2 rows x 3 columns)'; DataRows = 2 }
    )) {
        $headingIndex = [Array]::IndexOf($lines, $grid.Heading)
        if ($headingIndex -lt 0) {
            throw "Grid heading was missing: $($grid.Heading)"
        }

        $gridLines = @($lines[($headingIndex + 1)..($headingIndex + 4 + $grid.DataRows)])
        $lineWidth = $gridLines[0].Length
        foreach ($line in $gridLines) {
            if ($line.Length -ne $lineWidth) {
                throw "Grid lines have inconsistent widths under $($grid.Heading):`n$($gridLines -join "`n")"
            }
        }

        $contentLines = @($gridLines[1])
        if ($grid.DataRows -gt 0) {
            $contentLines += @($gridLines[3..(2 + $grid.DataRows)])
        }
        $dividerPositions = @(
            for ($position = 0; $position -lt $lineWidth; $position++) {
                if ($contentLines[0][$position] -eq [char]0x2502) {
                    $position
                }
            }
        )
        foreach ($line in $contentLines) {
            $positions = @(
                for ($position = 0; $position -lt $lineWidth; $position++) {
                    if ($line[$position] -eq [char]0x2502) {
                        $position
                    }
                }
            )
            if (($positions -join ',') -ne ($dividerPositions -join ',')) {
                throw "Grid dividers are misaligned under $($grid.Heading):`n$($gridLines -join "`n")"
            }
        }

        $borderJunctions = @(
            @{ Line = 0; Character = [char]0x252c },
            @{ Line = 2; Character = [char]0x253c },
            @{ Line = $gridLines.Length - 1; Character = [char]0x2534 }
        )
        foreach ($junction in $borderJunctions) {
            foreach ($position in $dividerPositions | Where-Object { $_ -gt 0 -and $_ -lt ($lineWidth - 1) }) {
                if ($gridLines[$junction.Line][$position] -ne $junction.Character) {
                    throw "Grid border junction is misaligned under $($grid.Heading):`n$($gridLines -join "`n")"
                }
            }
        }

        if ($gridLines[0][0] -ne [char]0x250c -or $gridLines[0][-1] -ne [char]0x2510 -or
            $gridLines[-1][0] -ne [char]0x2514 -or $gridLines[-1][-1] -ne [char]0x2518) {
            throw "Grid borders are not completely enclosed under $($grid.Heading):`n$($gridLines -join "`n")"
        }
    }

    $boxHeadings = @(
        'PILE empty_pile (size=0, capacity=4)',
        'QUEUE jobs (FIFO, size=2, capacity=8)',
        'QUEUE empty_queue (FIFO, size=0, capacity=8)'
    )
    $boxHeadings += @($lines | Where-Object { $_ -match '^PILE history \(size=\d+, capacity=\d+\)$' })
    foreach ($heading in $boxHeadings) {
        $headingIndex = [Array]::IndexOf($lines, $heading)
        $topIndex = $headingIndex + 1
        $bottomIndex = $topIndex + 1
        while ($bottomIndex -lt $lines.Length -and $lines[$bottomIndex][0] -ne [char]0x2514) {
            $bottomIndex++
        }
        if ($headingIndex -lt 0 -or $bottomIndex -ge $lines.Length) {
            throw "Box was not completely enclosed for $heading"
        }

        $boxLines = @($lines[$topIndex..$bottomIndex])
        $lineWidth = $boxLines[0].Length
        foreach ($line in $boxLines) {
            if ($line.Length -ne $lineWidth) {
                throw "Box is misaligned under ${heading}:`n$($boxLines -join "`n")"
            }
            if (($line[0] -ne [char]0x250c -and $line[0] -ne [char]0x2514 -and $line[0] -ne $vertical) -or
                ($line[-1] -ne [char]0x2510 -and $line[-1] -ne [char]0x2518 -and $line[-1] -ne $vertical)) {
                throw "Box has a broken vertical edge under ${heading}:`n$($boxLines -join "`n")"
            }
        }
        if ($boxLines[0][0] -ne [char]0x250c -or $boxLines[0][-1] -ne [char]0x2510 -or
            $boxLines[-1][0] -ne [char]0x2514 -or $boxLines[-1][-1] -ne [char]0x2518) {
            throw "Box borders are not completely enclosed under ${heading}:`n$($boxLines -join "`n")"
        }
    }

    Write-Output 'PASS: indexed table, persisted pile, matrix, FIFO queue, and file display'
}
finally {
    Remove-Item $stateFile, $dataFile, $executable -Force -ErrorAction SilentlyContinue
    Pop-Location
}