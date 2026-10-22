#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gc.h"
#include "value.h"

#define MAX_STACK_DEPTH 256
#define MAX_ACTION_PARAMETERS 16
#define MAX_CLASS_FIELDS 32
#define MAX_CLASSES 64
#define MAX_LINE_LENGTH 1024
#ifndef MAX_EXECUTION_STEPS
#define MAX_EXECUTION_STEPS 500000UL
#endif

typedef struct RuntimeContext RuntimeContext;

typedef struct
{
    char name[64];
    int is_string;
    int integer_value;
    char string_value[256];
} ClassField;

typedef struct
{
    char name[64];
    char parent_name[64];
    int parent_index;
    ClassField fields[MAX_CLASS_FIELDS];
    size_t field_count;
    int header_line;
    int body_start;
    int body_end;
    int closing_line;
} ClassDefinition;

typedef enum
{
    VARIABLE_COLLECTION_NONE,
    VARIABLE_COLLECTION_LIST,
    VARIABLE_COLLECTION_PILE
} VariableCollectionKind;

typedef enum
{
    VARIABLE_TYPE_UNDECLARED,
    VARIABLE_TYPE_INT,
    VARIABLE_TYPE_FLOAT,
    VARIABLE_TYPE_BOOL,
    VARIABLE_TYPE_STRING,
    VARIABLE_TYPE_NULL
} VariableDeclaredType;

typedef struct
{
    char name[64];
    char sval[256];
    int ival;
    int is_string;
    VariableDeclaredType declared_type;
    VariableCollectionKind collection_kind;
    int has_runtime_value;
    Value runtime_value;
} Variable;

typedef struct
{
    Variable *vars;
    size_t count;
    size_t capacity;
    RuntimeContext *runtime;
} Scope;

typedef struct
{
    char name[64];
    char parameters[MAX_ACTION_PARAMETERS][64];
    size_t parameter_count;
    int header_line;
    int start_line;
    int end_line;
    size_t entry_pc;
    char owner_class[64];
} Function;

typedef struct
{
    char severity[24];
    char code[32];
    char message[256];
    int line;
    size_t column;
    size_t span_length;
} RuntimeDiagnostic;

struct RuntimeContext
{
    GCHeap heap;
    bool initialized;
    uint64_t executed_instructions;
    size_t execution_steps;
    uint64_t random_state;
    Scope call_stack[MAX_STACK_DEPTH];
    int stack_top;
    Function *funcs;
    size_t func_count;
    size_t func_capacity;
    ClassDefinition classes[MAX_CLASSES];
    size_t class_count;
    char **source_lines;
    size_t source_line_count;
    size_t source_lines_capacity;
    char source_filename[256];
    int current_line;
    size_t diagnostic_count;
    RuntimeDiagnostic last_diagnostic;
};

#define RUNTIME_CONTEXT_INIT {0}

/* Initialize objects declared with RUNTIME_CONTEXT_INIT; a context cannot be
 * initialized twice without first being destroyed. */
bool runtime_init(RuntimeContext *runtime);
void runtime_destroy(RuntimeContext *runtime);
bool runtime_is_initialized(const RuntimeContext *runtime);
void runtime_record_instruction(RuntimeContext *runtime);
uint64_t runtime_get_executed_instructions(const RuntimeContext *runtime);
void runtime_reset_execution_steps(RuntimeContext *runtime);
bool runtime_consume_execution_step(RuntimeContext *runtime);
void runtime_random_seed(RuntimeContext *runtime, uint64_t seed);
uint64_t runtime_random_next(RuntimeContext *runtime);
void runtime_clear_source(RuntimeContext *runtime);

#endif
