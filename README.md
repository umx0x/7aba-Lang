# 7aba-Lang

7aba-Lang is an experimental, C99-based programming language with a
bytecode virtual machine. The source compiler and runtime are usable for
experiments and small programs, but the language is not yet a stable,
production-ready platform. In particular, the whole-program AST and resolver
are not the production compilation path, and the language has no stable
module/package system. A tagged release workflow is configured, but it has
not yet been verified on GitHub-hosted Linux and macOS runners.

## Build

Requirements:

- GCC with C99 support
- GNU Make is optional
- On non-Windows platforms, a C toolchain capable of linking `libdl` is needed
  for the native FFI implementation

On Windows, from the repository root:

```powershell
gcc -Wall -Wextra -Werror -O3 -std=c99 -o 7aba.exe main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c -lm
```

On Linux or other POSIX systems, add `-ldl`:

```sh
gcc -Wall -Wextra -Werror -O3 -std=c99 -o 7aba main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c -lm -ldl
```

If GNU Make is installed, `make` builds the interpreter with the same strict
warnings enabled.

## Run a program

```powershell
.\7aba.exe run .\pro.7ba
```

The compatible shorthand `.\7aba.exe .\pro.7ba` is also accepted. Use
`7aba --help` for the CLI usage. The supplied `pro.7ba` demonstrates lists,
indexing, loops, comparisons, and display.

Programs that use global variables or persisted collections may create a
sidecar file named `<script>.state`. This file is the program's persisted
runtime data; running the same script again can load and update that data.
Do not treat it as source code, and back it up before deleting or changing it.

## Website

The standalone website pages are kept in `website`. Open `website/index.html`
to view the home page or `website/download.html` for the available interpreter
downloads. Windows x86 and x64 prebuilt interpreters and a Linux/macOS source
package are provided. Linux and macOS users need a local C99 compiler to build
the source package. Release files and checksums are kept in `releases`.

## Publishing

The GitHub Pages workflow builds and checks the site when website or download
files change on `main`. To publish it, set **Settings → Pages → Build and
deployment → Source** to **GitHub Actions**. The Pages workflow also supports
manual runs from the Actions tab.

Pushing a `v*` tag starts the release workflow. It runs the C regression suite
on Windows x64, builds and smoke-tests the platform interpreters, then creates
a GitHub Release with packages and SHA-256 checksums. This workflow has not
run in GitHub Actions yet; verify every runner and asset before announcing a
public release. Until then, Linux and macOS downloads on the website are the
source package, not prebuilt interpreters.

## Test

Run the strict C unit tests and the PowerShell integration/regression scripts
from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\run_all.ps1
```

The runner requires GCC and PowerShell. It builds unit-test executables in the
system temporary directory and invokes every `tests/run_*.ps1` script. The
integration scripts cover collections and I/O, diagnostics, exceptions, GC,
input, OOP, FFI, and the VM benchmark.

The VS Code extension has its own test suite. From `vscode-extension`:

```powershell
npm ci
npm test
```

## Language and IDE references

- [Current language specification](./LANGUAGE_SPEC.md)
- [Architecture and implemented features](./ARCHITECTURE.md)
- [VS Code extension guide](./vscode-extension/README.md)

The specifications describe the implemented baseline and known limitations;
planned features should not be assumed to work.

## License

This repository is distributed under the MIT License. See [LICENSE](./LICENSE).
