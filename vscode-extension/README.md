# 7ABA Language Support

This VS Code extension registers `.7ba` files as the `7aba` language and provides TextMate syntax highlighting, formatting, `#` line comments, bracket matching, indentation rules, and a Language Server Protocol service. The language server reports unmatched delimiters, unterminated strings, malformed or duplicate action/class declarations, and provides document symbols plus definition navigation for declared actions and classes. It also adds a standalone play-icon Run button to the editor title bar for 7ABA files and bundles the Windows CLI runtime.

## Try the extension locally

From the repository root, run:

```sh
code --extensionDevelopmentPath=./vscode-extension .
```

Open a `.7ba` file in the launched Extension Development Host. Click the play button in the editor title bar to run the saved script. Output appears in the `7ABA` output channel. The grammar highlights control-flow keywords, collection and file commands, function definitions/calls, numbers, strings, comments, and operators.

The language server starts automatically for `.7ba` documents. Use **Go to Definition** on an action or class name to navigate to its declaration, or open the Outline view to browse declarations.

By default, the extension uses its bundled `bin/7aba.exe`. Set `7aba.executablePath` to an absolute path in VS Code settings to use a different executable.

To package it as a VS Code extension, install `@vscode/vsce` and run `vsce package` from this directory.

Run `npm test` in this directory to compile the extension and execute the language-service analysis tests.

## CLI

Build the interpreter from the repository root with GCC:

```sh
gcc -Wall -Wextra -Werror -O3 -std=c99 -o 7aba main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c
```

Run a script with:

```sh
./7aba run pro.7ba
```

The backwards-compatible form `./7aba pro.7ba` is also supported. On Windows, use `7aba.exe` in place of `./7aba`.
