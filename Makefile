CC = gcc
CFLAGS = -Wall -Wextra -Werror -O3 -std=c99
TARGET = 7aba

SRCS = main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c
OBJS = $(SRCS:.c=.o)
LDLIBS = -lm

ifeq ($(OS),Windows_NT)
FFI_FIXTURE = tests\ffi_fixture.dll
RM = cmd /c del /Q
else
LDLIBS += -ldl
FFI_FIXTURE = tests/ffi_fixture.so
RM = rm -f
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

%.o: %.c main.h bytecode.h diagnostic.h value.h gc.h runtime.h stdlib.h ffi.h ast.h typecheck.h program_ast.h resolver.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS) $(TARGET)

test-value:
	$(CC) $(CFLAGS) -o tests/value_test.exe tests/value_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/value_test.exe

test-ast:
	$(CC) $(CFLAGS) -o tests/ast_test.exe tests/ast_test.c ast.c
	./tests/ast_test.exe

test-program-ast:
	$(CC) $(CFLAGS) -o tests/program_ast_test.exe program_ast.c ast.c tests/program_ast_test.c
	./tests/program_ast_test.exe

test-resolver:
	$(CC) $(CFLAGS) -o tests/resolver_test.exe resolver.c program_ast.c ast.c tests/resolver_test.c
	./tests/resolver_test.exe

test-typecheck:
	$(CC) $(CFLAGS) -o tests/typecheck_test.exe tests/typecheck_test.c typecheck.c ast.c
	./tests/typecheck_test.exe

test-type-annotations:
	$(CC) $(CFLAGS) -o tests/type_annotations_test.exe tests/type_annotations_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/type_annotations_test.exe

test-ast-bytecode:
	$(CC) $(CFLAGS) -o tests/ast_bytecode_test.exe tests/ast_bytecode_test.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c lexer.c parser.c eval.c $(LDLIBS)
	./tests/ast_bytecode_test.exe

test-runtime-context:
	$(CC) $(CFLAGS) -Werror -o tests/runtime_context_test.exe tests/runtime_context_test.c eval.c lexer.c parser.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/runtime_context_test.exe

test-scope-gc:
	$(CC) $(CFLAGS) -Werror -o tests/scope_gc_test.exe tests/scope_gc_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/scope_gc_test.exe

test-gc-safe-point:
	$(CC) $(CFLAGS) -Werror -o tests/gc_safe_point_test.exe tests/gc_safe_point_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/gc_safe_point_test.exe

benchmark-vm:
	$(CC) $(CFLAGS) -Werror -o tests/vm_benchmark.exe tests/vm_benchmark.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/vm_benchmark.exe

test-stdlib-ffi:
	$(CC) $(CFLAGS) -Werror -shared -o $(FFI_FIXTURE) tests/ffi_fixture.c
	$(CC) $(CFLAGS) -o tests/stdlib_ffi_test.exe tests/stdlib_ffi_test.c lexer.c parser.c eval.c bytecode.c diagnostic.c stdlib.c value.c gc.c runtime.c ffi.c ast.c typecheck.c $(LDLIBS)
	./tests/stdlib_ffi_test.exe $(FFI_FIXTURE)
	$(RM) $(FFI_FIXTURE) tests\stdlib_ffi_test.exe

run: all
	./$(TARGET) run pro.7ba

.PHONY: all clean run test-ast test-program-ast test-resolver test-typecheck test-type-annotations test-ast-bytecode test-value test-runtime-context test-scope-gc test-gc-safe-point benchmark-vm test-stdlib-ffi
