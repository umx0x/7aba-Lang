#ifndef BYTECODE_H
#define BYTECODE_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime.h"
#include "value.h"

typedef enum
{
    BC_PUSH_INT,
    BC_PUSH_BOOL,
    BC_PUSH_NULL,
    BC_PUSH_STRING,
    BC_LOAD_VAR,
    BC_DECLARE_VAR,
    BC_LIST_GET,
    BC_BUILD_MAP,
    BC_BUILD_SET,
    BC_MAP_SET,
    BC_STORE_VAR,
    BC_INPUT,
    BC_MATRIX_CREATE,
    BC_MATRIX_SET,
    BC_MATRIX_GET,
    BC_QUEUE_CREATE,
    BC_QUEUE_ENQUEUE,
    BC_QUEUE_DEQUEUE,
    BC_QUEUE_PEEK,
    BC_COLLECTION_LEN,
    BC_LIST_CREATE,
    BC_PILE_CREATE,
    BC_COLLECTION_PUSH,
    BC_COLLECTION_POP,
    BC_COLLECTION_SAVE,
    BC_COLLECTION_LOAD,
    BC_FILE_DISPLAY,
    BC_FILE_READ,
    BC_FILE_WRITE,
    BC_CONTAINS,
    BC_DISPLAY_VALUE,
    BC_DISPLAY_SPECIAL,
    BC_DISPLAY_NEWLINE,
    BC_LOAD_FIELD,
    BC_STORE_FIELD,
    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_NEGATE,
    BC_COMPARE,
    BC_STDLIB_CALL,
    BC_JUMP,
    BC_JUMP_IF_FALSE,
    BC_REPEAT_INIT,
    BC_REPEAT_NEXT,
    BC_CALL,
    BC_METHOD_CALL,
    BC_NEW,
    BC_TRY,
    BC_END_TRY,
    BC_THROW,
    BC_RETURN,
    BC_HALT
} BytecodeOpcode;

typedef enum
{
    BC_STDLIB_ABS,
    BC_STDLIB_MIN,
    BC_STDLIB_MAX,
    BC_STDLIB_LEN,
    BC_STDLIB_LOWER,
    BC_STDLIB_UPPER,
    BC_STDLIB_SUBSTR,
    BC_STDLIB_TRIM,
    BC_STDLIB_STARTS_WITH,
    BC_STDLIB_ENDS_WITH,
    BC_STDLIB_SQRT,
    BC_STDLIB_SIN,
    BC_STDLIB_COS,
    BC_STDLIB_LOG,
    BC_STDLIB_EXP,
    BC_STDLIB_FLOOR,
    BC_STDLIB_CEIL,
    BC_STDLIB_ROUND,
    BC_STDLIB_PI,
    BC_STDLIB_RANDOM_INT,
    BC_STDLIB_RANDOM_SEED,
    BC_STDLIB_CONTAINS,
    BC_STDLIB_INDEX_OF,
    BC_STDLIB_REPLACE,
    BC_STDLIB_REPEAT,
    BC_STDLIB_COUNT
} BytecodeStdlibFunction;

typedef struct
{
    BytecodeOpcode opcode;
    int operand;
    int operand2;
    int operand3;
    int line;
    size_t column;
    size_t span_length;
} BytecodeInstruction;

typedef struct
{
    RuntimeContext *runtime;
    BytecodeInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;
    char **symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    bool has_result;
} BytecodeProgram;

#define BYTECODE_PROGRAM_INIT {NULL, NULL, 0, 0, NULL, 0, 0, false}

bool bytecode_compile_expression(RuntimeContext *runtime, const char *source,
                                 BytecodeProgram *program);
/* Compatibility entry point; production expression compilation uses the same
 * AST parser and lowering path. */
bool bytecode_compile_ast_expression(RuntimeContext *runtime, const char *source,
                                    BytecodeProgram *program);
bool bytecode_compile_program(RuntimeContext *runtime, int first_line,
                              int last_line, BytecodeProgram *program);
bool bytecode_execute(RuntimeContext *runtime, const BytecodeProgram *program,
                      int *result);
bool bytecode_execute_value(RuntimeContext *runtime, const BytecodeProgram *program,
                            Value *result);
void bytecode_free(BytecodeProgram *program);
bool bytecode_evaluate_expression(RuntimeContext *runtime, const char *source,
                                  int *result);
bool bytecode_evaluate_value(RuntimeContext *runtime, const char *source,
                             Value *result);

#endif