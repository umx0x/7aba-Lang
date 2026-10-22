#!/bin/sh
set -eu

CC=${CC:-cc}
SYSTEM=$(uname -s)

case "$SYSTEM" in
    Linux)
        PLATFORM_LIBS="-lm -ldl"
        ;;
    Darwin)
        PLATFORM_LIBS="-lm"
        ;;
    *)
        printf 'Unsupported build platform: %s\n' "$SYSTEM" >&2
        exit 1
        ;;
esac

"$CC" -Wall -Wextra -Werror -O3 -std=c99 \
    -o 7aba \
    main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c \
    gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c \
    $PLATFORM_LIBS
