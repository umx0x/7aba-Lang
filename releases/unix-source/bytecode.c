#include "main.h"
#include "bytecode.h"
#include "diagnostic.h"
#include "ast.h"
#include "stdlib.h"
#include "typecheck.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

typedef struct
{
    RuntimeContext *runtime;
    const char *source;
    const char *cursor;
    BytecodeProgram *program;
    bool failed;
    size_t source_column_offset;
    int source_line_offset;
} Compiler;

typedef struct
{
    Value *values;
    size_t depth;
} VMStackRoot;

typedef struct
{
    int remaining;
    size_t body_pc;
} RepeatFrame;

typedef struct
{
    size_t return_pc;
    size_t stack_depth;
    size_t repeat_depth;
    size_t exception_depth;
} CallFrame;

typedef struct
{
    size_t catch_pc;
    size_t stack_depth;
    size_t repeat_depth;
    size_t call_depth;
    int scope_top;
    int exception_symbol;
} ExceptionFrame;

static void compiler_error(Compiler *compiler, const char *message)
{
    if (compiler->failed)
        return;
    size_t column = compiler->source_column_offset +
                    (size_t)(compiler->cursor - compiler->source) + 1;
    size_t span_length = 0;
    const char *cursor = compiler->cursor;
    if (isalnum((unsigned char)*cursor) || *cursor == '_')
    {
        while (isalnum((unsigned char)*cursor) || *cursor == '_')
        {
            span_length++;
            cursor++;
        }
    }
    else if (*cursor)
    {
        span_length = 1;
        if ((cursor[0] == '=' && cursor[1] == '=') ||
            (cursor[0] == '!' && cursor[1] == '=') ||
            (cursor[0] == '<' && cursor[1] == '=') ||
            (cursor[0] == '>' && cursor[1] == '='))
            span_length = 2;
    }
    else
        span_length = 1;
    report_diagnostic_span(compiler->runtime, "error", "E1001", message,
                           compiler->runtime->current_line, column, span_length);
    compiler->failed = true;
}

static void trace_vm_stack(void *root, GCMarkObject mark, void *mark_context)
{
    VMStackRoot *stack_root = root;
    for (size_t i = 0; i < stack_root->depth; i++)
        value_trace(&stack_root->values[i], mark, mark_context);
}

static int operand_integer(const Value *operand)
{
    if (!operand || operand->type != VALUE_INT ||
        operand->as.integer < INT_MIN || operand->as.integer > INT_MAX)
        return 0;
    return (int)operand->as.integer;
}

static bool operand_size(const Value *operand, size_t *result)
{
    if (!operand || operand->type != VALUE_INT || !result ||
        operand->as.integer < 0 ||
        (uint64_t)operand->as.integer > (uint64_t)SIZE_MAX)
        return false;
    *result = (size_t)operand->as.integer;
    return true;
}

static void print_value(const Value *value)
{
    switch (value->type)
    {
    case VALUE_NULL:
        fputs("null", stdout);
        break;
    case VALUE_INT:
        printf("%lld", (long long)value->as.integer);
        break;
    case VALUE_FLOAT:
        printf("%.17g", value->as.floating);
        break;
    case VALUE_BOOL:
        fputs(value->as.boolean ? "true" : "false", stdout);
        break;
    case VALUE_STRING:
    {
        size_t length = 0;
        const char *bytes = value_string_bytes(value, &length);
        if (bytes)
            fwrite(bytes, 1, length, stdout);
        break;
    }
    case VALUE_ARRAY:
        printf("[array length=%lu]", (unsigned long)value_array_length(value));
        break;
    case VALUE_MATRIX:
        printf("[matrix %lux%lu]",
               (unsigned long)value_matrix_rows(value),
               (unsigned long)value_matrix_columns(value));
        break;
    case VALUE_QUEUE:
        printf("[queue length=%lu]", (unsigned long)value_queue_length(value));
        break;
    case VALUE_MAP:
        printf("[map length=%lu]", (unsigned long)value_map_length(value));
        break;
    case VALUE_SET:
        printf("[set length=%lu]", (unsigned long)value_set_length(value));
        break;
    case VALUE_OBJECT:
        fputs("[object]", stdout);
        break;
    case VALUE_NATIVE:
        fputs("[native]", stdout);
        break;
    }
}

static bool load_variable_value(RuntimeContext *runtime, const Variable *variable,
                                Value *result)
{
    if (!variable)
    {
        *result = value_int(0);
        return true;
    }
    if (variable->has_runtime_value)
        return value_copy(result, &variable->runtime_value);
    if (variable->is_string)
        return value_string(runtime, variable->sval, strlen(variable->sval), result);
    *result = value_int(variable->ival);
    return true;
}

static bool bind_argument(RuntimeContext *runtime, const char *name,
                          const Value *argument)
{
    if (!name || !argument)
        return false;
    Variable *parameter = get_or_create_var(runtime, name);
    return variable_replace_runtime_value(parameter, argument);
}

static void runtime_error(RuntimeContext *runtime, size_t column,
                          const char *message)
{
    report_diagnostic(runtime, "runtime error", "E2001", message,
                      runtime->current_line, column);
}

static void runtime_error_span(RuntimeContext *runtime, size_t column,
                               size_t span_length,
                               const char *message)
{
    report_diagnostic_span(runtime, "runtime error", "E2001", message,
                           runtime->current_line, column, span_length);
}

static bool parse_input_names(char *source, char *names[], size_t capacity, size_t *count)
{
    *count = 0;
    while (*source)
    {
        if (*count >= capacity)
            return false;
        char *name = source;
        if (!(isalpha((unsigned char)*source) || *source == '_'))
            return false;
        source++;
        while (isalnum((unsigned char)*source) || *source == '_')
            source++;
        size_t length = (size_t)(source - name);
        if (length >= 64 || (*source && *source != ' '))
            return false;
        names[(*count)++] = name;
        if (*source)
            *source++ = '\0';
    }
    return *count != 0;
}

static bool parse_input_values(char *source, char *values[], size_t capacity,
                               size_t *count)
{
    *count = 0;
    while (*source)
    {
        while (isspace((unsigned char)*source))
            source++;
        if (!*source)
            break;
        if (*count >= capacity)
            return false;

        char *value;
        if (*source == '"')
        {
            value = ++source;
            while (*source && *source != '"')
                source++;
            if (!*source)
                return false;
            *source++ = '\0';
            if (*source && !isspace((unsigned char)*source))
                return false;
        }
        else
        {
            value = source;
            while (*source && !isspace((unsigned char)*source))
                source++;
            if (*source)
                *source++ = '\0';
        }
        if (strlen(value) >= 256)
            return false;
        values[(*count)++] = value;
    }
    return true;
}

static void skip_whitespace(Compiler *compiler)
{
    while (isspace((unsigned char)*compiler->cursor))
        compiler->cursor++;
}

static bool compile_ast_into(BytecodeProgram *program, const char *source,
                             int source_line_offset, size_t source_column_offset,
                             bool allow_block_opener, bool emit_halt);

static bool grow_array(void **array, size_t *capacity, size_t element_size)
{
    size_t next_capacity = *capacity == 0 ? 8 : *capacity * 2;
    if (next_capacity < *capacity || next_capacity > SIZE_MAX / element_size)
        return false;
    void *next = realloc(*array, next_capacity * element_size);
    if (!next)
        return false;
    *array = next;
    *capacity = next_capacity;
    return true;
}

static bool emit_span(Compiler *compiler, BytecodeOpcode opcode, int operand,
                      size_t column, size_t span_length)
{
    BytecodeProgram *program = compiler->program;
    if (program->instruction_count == program->instruction_capacity &&
        !grow_array((void **)&program->instructions, &program->instruction_capacity,
                    sizeof(*program->instructions)))
    {
        compiler_error(compiler, "Unable to allocate expression bytecode.");
        return false;
    }
    BytecodeInstruction *instruction = &program->instructions[program->instruction_count++];
    instruction->opcode = opcode;
    instruction->operand = operand;
    instruction->operand2 = 0;
    instruction->operand3 = 0;
    instruction->line = compiler->runtime->current_line;
    instruction->column = compiler->source_column_offset + column;
    instruction->span_length = span_length ? span_length : 1;
    return true;
}

static bool emit(Compiler *compiler, BytecodeOpcode opcode, int operand, size_t column)
{
    return emit_span(compiler, opcode, operand, column, 1);
}

static int intern_symbol(Compiler *compiler, const char *name, size_t length)
{
    BytecodeProgram *program = compiler->program;
    for (size_t i = 0; i < program->symbol_count; i++)
    {
        if (strlen(program->symbols[i]) == length &&
            memcmp(program->symbols[i], name, length) == 0)
            return (int)i;
    }

    if (program->symbol_count >= (size_t)INT_MAX)
    {
        compiler_error(compiler, "Expression contains too many identifiers.");
        return -1;
    }
    if (program->symbol_count == program->symbol_capacity &&
        !grow_array((void **)&program->symbols, &program->symbol_capacity,
                    sizeof(*program->symbols)))
    {
        compiler_error(compiler, "Unable to allocate expression symbols.");
        return -1;
    }

    char *symbol = malloc(length + 1);
    if (!symbol)
    {
        compiler_error(compiler, "Unable to allocate an expression identifier.");
        return -1;
    }
    memcpy(symbol, name, length);
    symbol[length] = '\0';
    program->symbols[program->symbol_count] = symbol;
    return (int)program->symbol_count++;
}

static bool parse_expression(Compiler *compiler);

static int stdlib_function_id(const char *name, size_t length)
{
    static const struct
    {
        const char *name;
        BytecodeStdlibFunction id;
    } functions[] = {
        {"abs", BC_STDLIB_ABS},
        {"min", BC_STDLIB_MIN},
        {"max", BC_STDLIB_MAX},
        {"len", BC_STDLIB_LEN},
        {"lower", BC_STDLIB_LOWER},
        {"upper", BC_STDLIB_UPPER},
        {"substr", BC_STDLIB_SUBSTR},
        {"trim", BC_STDLIB_TRIM},
        {"starts_with", BC_STDLIB_STARTS_WITH},
        {"ends_with", BC_STDLIB_ENDS_WITH},
        {"sqrt", BC_STDLIB_SQRT},
        {"sin", BC_STDLIB_SIN},
        {"cos", BC_STDLIB_COS},
        {"log", BC_STDLIB_LOG},
        {"exp", BC_STDLIB_EXP},
        {"floor", BC_STDLIB_FLOOR},
        {"ceil", BC_STDLIB_CEIL},
        {"round", BC_STDLIB_ROUND},
        {"pi", BC_STDLIB_PI},
        {"random_int", BC_STDLIB_RANDOM_INT},
        {"random_seed", BC_STDLIB_RANDOM_SEED},
        {"contains", BC_STDLIB_CONTAINS},
        {"index_of", BC_STDLIB_INDEX_OF},
        {"replace", BC_STDLIB_REPLACE},
        {"repeat", BC_STDLIB_REPEAT},
        {"count", BC_STDLIB_COUNT}};
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++)
        if (strlen(functions[i].name) == length &&
            memcmp(functions[i].name, name, length) == 0)
            return (int)functions[i].id;
    return -1;
}

static bool parse_stdlib_call(Compiler *compiler, int function_id, size_t column,
                              size_t function_name_length)
{
    if (*compiler->cursor != '(')
        return false;
    compiler->cursor++;
    skip_whitespace(compiler);
    int argument_count = 0;
    if (*compiler->cursor != ')')
    {
        for (;;)
        {
            if (argument_count >= 3 || !parse_expression(compiler))
            {
                if (!compiler->failed)
                    compiler_error(compiler, "Standard-library calls accept at most three arguments.");
                return false;
            }
            argument_count++;
            skip_whitespace(compiler);
            if (*compiler->cursor != ',')
                break;
            compiler->cursor++;
        }
    }
    if (*compiler->cursor != ')')
    {
        compiler_error(compiler, "Expected ')' after standard-library arguments.");
        return false;
    }
    compiler->cursor++;

    int expected_arguments =
        function_id == BC_STDLIB_PI ? 0
        : function_id == BC_STDLIB_ABS || function_id == BC_STDLIB_LEN ||
                  function_id == BC_STDLIB_LOWER ||
                  function_id == BC_STDLIB_UPPER ||
                  function_id == BC_STDLIB_TRIM ||
                  (function_id >= BC_STDLIB_SQRT &&
                   function_id <= BC_STDLIB_ROUND) ||
                  function_id == BC_STDLIB_RANDOM_SEED
            ? 1
        : function_id == BC_STDLIB_SUBSTR ||
                  function_id == BC_STDLIB_REPLACE
            ? 3
                                          : 2;
    if (argument_count != expected_arguments)
    {
        compiler_error(compiler, "Incorrect number of standard-library arguments.");
        return false;
    }
    if (!emit_span(compiler, BC_STDLIB_CALL, function_id, column,
                   function_name_length))
        return false;
    compiler->program->instructions[compiler->program->instruction_count - 1].operand2 =
        argument_count;
    return true;
}

static bool parse_collection_literal(Compiler *compiler, bool force_set, size_t column)
{
    if (!force_set)
    {
        if (*compiler->cursor != '{')
            return false;
        compiler->cursor++;
    }
    else
    {
        skip_whitespace(compiler);
        if (*compiler->cursor != '{')
            return false;
        compiler->cursor++;
    }

    skip_whitespace(compiler);
    if (*compiler->cursor == '}')
    {
        compiler->cursor++;
        return emit(compiler, force_set ? BC_BUILD_SET : BC_BUILD_MAP, 0, column);
    }

    size_t count = 0;
    bool is_map = !force_set;
    if (!parse_expression(compiler))
        return false;
    skip_whitespace(compiler);
    if (is_map && *compiler->cursor != ':')
        is_map = false;

    if (is_map)
    {
        do
        {
            skip_whitespace(compiler);
            if (*compiler->cursor != ':')
            {
                compiler_error(compiler, "Expected ':' between map key and value.");
                return false;
            }
            compiler->cursor++;
            if (!parse_expression(compiler))
                return false;
            if (count == (size_t)INT_MAX)
            {
                compiler_error(compiler, "Map literal contains too many entries.");
                return false;
            }
            count++;
            skip_whitespace(compiler);
            if (*compiler->cursor != ',')
                break;
            compiler->cursor++;
            skip_whitespace(compiler);
            if (*compiler->cursor == '}')
                break;
            if (!parse_expression(compiler))
                return false;
        } while (true);
    }
    else
    {
        count = 1;
        while (true)
        {
            skip_whitespace(compiler);
            if (*compiler->cursor != ',')
                break;
            compiler->cursor++;
            skip_whitespace(compiler);
            if (*compiler->cursor == '}')
                break;
            if (count == (size_t)INT_MAX || !parse_expression(compiler))
            {
                if (!compiler->failed)
                    compiler_error(compiler, "Set literal contains too many elements.");
                return false;
            }
            count++;
        }
    }

    skip_whitespace(compiler);
    if (*compiler->cursor != '}')
    {
        compiler_error(compiler, "Expected '}' to close collection literal.");
        return false;
    }
    compiler->cursor++;
    return emit(compiler, is_map ? BC_BUILD_MAP : BC_BUILD_SET,
                (int)count, column);
}

static bool parse_primary(Compiler *compiler)
{
    skip_whitespace(compiler);
    size_t column = (size_t)(compiler->cursor - compiler->source) + 1;

    if (*compiler->cursor == '(')
    {
        compiler->cursor++;
        if (!parse_expression(compiler))
            return false;
        skip_whitespace(compiler);
        if (*compiler->cursor != ')')
        {
            compiler_error(compiler, "Expected ')' to close expression.");
            return false;
        }
        compiler->cursor++;
        return true;
    }

    if (*compiler->cursor == '{')
        return parse_collection_literal(compiler, false, column);

    if (*compiler->cursor == '"')
    {
        compiler->cursor++;
        char *literal = malloc(strlen(compiler->cursor) + 1);
        if (!literal)
        {
            compiler_error(compiler, "Unable to allocate string literal.");
            return false;
        }
        size_t length = 0;
        while (*compiler->cursor && *compiler->cursor != '"')
        {
            char value = *compiler->cursor++;
            if (value == '\\' && *compiler->cursor)
            {
                char escaped = *compiler->cursor++;
                value = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
            }
            literal[length++] = value;
        }
        if (*compiler->cursor != '"')
        {
            free(literal);
            compiler_error(compiler, "Unterminated string literal.");
            return false;
        }
        compiler->cursor++;
        int symbol = intern_symbol(compiler, literal, length);
        free(literal);
        return symbol >= 0 && emit(compiler, BC_PUSH_STRING, symbol, column);
    }

    if (isdigit((unsigned char)*compiler->cursor))
    {
        errno = 0;
        char *end = NULL;
        long value = strtol(compiler->cursor, &end, 10);
        if (errno == ERANGE || value < INT_MIN || value > INT_MAX)
        {
            compiler_error(compiler, "Integer literal is outside the supported range.");
            return false;
        }
        compiler->cursor = end;
        return emit(compiler, BC_PUSH_INT, (int)value, column);
    }

    if (isalpha((unsigned char)*compiler->cursor) || *compiler->cursor == '_')
    {
        const char *name_start = compiler->cursor++;
        while (isalnum((unsigned char)*compiler->cursor) || *compiler->cursor == '_')
            compiler->cursor++;
        size_t name_length = (size_t)(compiler->cursor - name_start);
        skip_whitespace(compiler);
        if (name_length == 3 && memcmp(name_start, "set", 3) == 0 &&
            *compiler->cursor == '{')
            return parse_collection_literal(compiler, true, column);
        if (name_length == 4 && memcmp(name_start, "true", 4) == 0)
            return emit(compiler, BC_PUSH_BOOL, 1, column);
        if (name_length == 5 && memcmp(name_start, "false", 5) == 0)
            return emit(compiler, BC_PUSH_BOOL, 0, column);
        if (name_length == 4 && memcmp(name_start, "null", 4) == 0)
            return emit(compiler, BC_PUSH_NULL, 0, column);
        if (name_length >= 64)
        {
            compiler_error(compiler, "Identifier exceeds the 63-character limit.");
            return false;
        }
        int builtin_id = stdlib_function_id(name_start, name_length);
        if (builtin_id >= 0 && *compiler->cursor == '(')
            return parse_stdlib_call(compiler, builtin_id, column, name_length);
        int symbol = intern_symbol(compiler, name_start, name_length);
        if (symbol < 0)
            return false;

        skip_whitespace(compiler);
        if (*compiler->cursor == '.')
        {
            compiler->cursor++;
            skip_whitespace(compiler);
            const char *field_start = compiler->cursor;
            if (!(isalpha((unsigned char)*field_start) || *field_start == '_'))
            {
                compiler_error(compiler, "Expected a property name after '.'.");
                return false;
            }
            compiler->cursor++;
            while (isalnum((unsigned char)*compiler->cursor) || *compiler->cursor == '_')
                compiler->cursor++;
            size_t field_length = (size_t)(compiler->cursor - field_start);
            if (field_length >= 64)
            {
                compiler_error(compiler, "Property name exceeds the 63-character limit.");
                return false;
            }
            char property[129];
            snprintf(property, sizeof(property), "%.*s.%.*s",
                     (int)name_length, name_start, (int)field_length, field_start);
            int field_symbol = intern_symbol(compiler, property, strlen(property));
            return field_symbol >= 0 && emit(compiler, BC_LOAD_FIELD, field_symbol, column);
        }
        if (*compiler->cursor == '[')
        {
            const char *indexed_start = name_start;
            compiler->cursor++;
            if (!parse_expression(compiler))
                return false;
            skip_whitespace(compiler);
            if (*compiler->cursor != ']')
            {
                compiler_error(compiler, "Expected ']' after list index.");
                return false;
            }
            compiler->cursor++;
            return emit_span(compiler, BC_LIST_GET, symbol, column,
                             (size_t)(compiler->cursor - indexed_start));
        }
        return emit_span(compiler, BC_LOAD_VAR, symbol, column, name_length);
    }

    compiler_error(compiler, "Expected an integer, identifier, or parenthesized expression.");
    return false;
}

static bool parse_unary(Compiler *compiler)
{
    skip_whitespace(compiler);
    if (*compiler->cursor == '-')
    {
        size_t column = (size_t)(compiler->cursor - compiler->source) + 1;
        compiler->cursor++;
        return parse_unary(compiler) && emit(compiler, BC_NEGATE, 0, column);
    }
    return parse_primary(compiler);
}

static bool parse_term(Compiler *compiler)
{
    if (!parse_unary(compiler))
        return false;
    while (true)
    {
        skip_whitespace(compiler);
        char operator = *compiler->cursor;
        if (operator != '*' && operator != '/')
            return true;
        size_t column = (size_t)(compiler->cursor - compiler->source) + 1;
        compiler->cursor++;
        if (!parse_unary(compiler))
            return false;
        if (!emit(compiler, operator == '*' ? BC_MUL : BC_DIV, 0, column))
            return false;
    }
}

static bool parse_sum(Compiler *compiler)
{
    if (!parse_term(compiler))
        return false;
    while (true)
    {
        skip_whitespace(compiler);
        char operator = *compiler->cursor;
        if (operator != '+' && operator != '-')
            break;
        size_t column = (size_t)(compiler->cursor - compiler->source) + 1;
        compiler->cursor++;
        if (!parse_term(compiler))
            return false;
        if (!emit(compiler, operator == '+' ? BC_ADD : BC_SUB, 0, column))
            return false;
    }

    return true;
}

static bool parse_expression(Compiler *compiler)
{
    if (!parse_sum(compiler))
        return false;

    skip_whitespace(compiler);
    if (strncmp(compiler->cursor, "in", 2) == 0 &&
        !(isalnum((unsigned char)compiler->cursor[2]) || compiler->cursor[2] == '_'))
    {
        size_t column = (size_t)(compiler->cursor - compiler->source) + 1;
        compiler->cursor += 2;
        if (!parse_sum(compiler) || !emit(compiler, BC_CONTAINS, 0, column))
            return false;
    }

    skip_whitespace(compiler);
    ValueBinaryOp operation;
    size_t column = (size_t)(compiler->cursor - compiler->source) + 1;
    size_t operator_length = 1;
    if (strncmp(compiler->cursor, "==", 2) == 0)
    {
        operation = VALUE_OP_EQUAL;
        operator_length = 2;
        compiler->cursor += 2;
    }
    else if (strncmp(compiler->cursor, "!=", 2) == 0)
    {
        operation = VALUE_OP_NOT_EQUAL;
        operator_length = 2;
        compiler->cursor += 2;
    }
    else if (strncmp(compiler->cursor, "<=", 2) == 0)
    {
        operation = VALUE_OP_LESS_EQUAL;
        operator_length = 2;
        compiler->cursor += 2;
    }
    else if (strncmp(compiler->cursor, ">=", 2) == 0)
    {
        operation = VALUE_OP_GREATER_EQUAL;
        operator_length = 2;
        compiler->cursor += 2;
    }
    else if (*compiler->cursor == '<')
    {
        operation = VALUE_OP_LESS;
        compiler->cursor++;
    }
    else if (*compiler->cursor == '>')
    {
        operation = VALUE_OP_GREATER;
        compiler->cursor++;
    }
    else
    {
        return true;
    }

    if (!parse_sum(compiler))
        return false;
    return emit_span(compiler, BC_COMPARE, (int)operation, column, operator_length);
}

static bool emit_expression_into(BytecodeProgram *program, const char *source, int line)
{
    if (!source || !program || !program->runtime)
        return false;
    size_t column_offset = 0;
    if (line >= 0 && (size_t)line < program->runtime->source_line_count &&
        program->runtime->source_lines &&
        program->runtime->source_lines[line])
    {
        const char *found = strstr(program->runtime->source_lines[line], source);
        if (found)
            column_offset = (size_t)(found - program->runtime->source_lines[line]);
    }
    return compile_ast_into(program, source, line, column_offset, false, false);
}

void bytecode_free(BytecodeProgram *program)
{
    if (!program)
        return;
    for (size_t i = 0; i < program->symbol_count; i++)
        free(program->symbols[i]);
    free(program->symbols);
    free(program->instructions);
    memset(program, 0, sizeof(*program));
}

bool bytecode_compile_expression(RuntimeContext *runtime, const char *source,
                                 BytecodeProgram *program)
{
    if (!runtime_is_initialized(runtime) || !source || !program)
        return false;
    memset(program, 0, sizeof(*program));
    program->runtime = runtime;
    if (!compile_ast_into(program, source, runtime->current_line - 1, 0,
                          true, true))
    {
        bytecode_free(program);
        return false;
    }
    return true;
}

static const char *ast_function_name(const AstNode *node)
{
    return node->as.call.name;
}

static size_t ast_source_offset(const char *source, size_t line, size_t column)
{
    size_t offset = 0;
    size_t current_line = 1;
    size_t current_column = 1;
    while (source[offset] && (current_line < line || current_column < column))
    {
        if (source[offset++] == '\n')
        {
            current_line++;
            current_column = 1;
        }
        else
            current_column++;
    }
    return offset;
}

static bool emit_ast_instruction(Compiler *compiler, const AstNode *node,
                                 BytecodeOpcode opcode, int operand,
                                 size_t span_length)
{
    size_t offset = ast_source_offset(compiler->source, node->span.line,
                                      node->span.column);
    Compiler located = *compiler;
    located.cursor = compiler->source + offset;
    located.source_column_offset = node->span.line == 1
                                       ? compiler->source_column_offset
                                       : 0;
    int previous_line = compiler->runtime->current_line;
    located.runtime->current_line = located.source_line_offset +
                                    (int)node->span.line;
    size_t length = span_length;
    if (!length && node->span.end_line == node->span.line &&
        node->span.end_column > node->span.column)
        length = node->span.end_column - node->span.column;
    if (!length)
        length = 1;
    bool success = emit_span(&located, opcode, operand, node->span.column, length);
    compiler->runtime->current_line = previous_line;
    return success;
}

static bool emit_ast_operator(Compiler *compiler, const AstNode *node,
                              BytecodeOpcode opcode, int operand)
{
    const AstNode *left = node->as.binary.left;
    const AstNode *right = node->as.binary.right;
    size_t offset = ast_source_offset(compiler->source, left->span.end_line,
                                      left->span.end_column);
    size_t end = ast_source_offset(compiler->source, right->span.line,
                                   right->span.column);
    while (offset < end && isspace((unsigned char)compiler->source[offset]))
        offset++;
    size_t length = node->as.binary.operation == AST_OP_IN ? 2 :
                    (offset + 1 < end && compiler->source[offset + 1] == '=' ? 2 : 1);
    size_t line = 1;
    size_t column = 1;
    for (size_t i = 0; i < offset; i++)
    {
        if (compiler->source[i] == '\n')
        {
            line++;
            column = 1;
        }
        else
            column++;
    }
    Compiler located = *compiler;
    located.cursor = compiler->source + offset;
    located.source_column_offset = line == 1
                                       ? compiler->source_column_offset
                                       : 0;
    int previous_line = compiler->runtime->current_line;
    located.runtime->current_line = located.source_line_offset + (int)line;
    bool success = emit_span(&located, opcode, operand, column, length);
    compiler->runtime->current_line = previous_line;
    return success;
}

static bool lower_ast_node(Compiler *compiler, const AstNode *node)
{
    int symbol;
    switch (node->kind)
    {
    case AST_INTEGER:
        return emit_ast_instruction(compiler, node, BC_PUSH_INT,
                                    node->as.integer, 1);
    case AST_BOOLEAN:
        return emit_ast_instruction(compiler, node, BC_PUSH_BOOL,
                                    node->as.boolean ? 1 : 0, 1);
    case AST_NULL:
        return emit_ast_instruction(compiler, node, BC_PUSH_NULL, 0, 1);
    case AST_STRING:
        symbol = intern_symbol(compiler, node->as.string.bytes,
                               node->as.string.length);
        return symbol >= 0 &&
               emit_ast_instruction(compiler, node, BC_PUSH_STRING, symbol, 1);
    case AST_NAME:
        symbol = intern_symbol(compiler, node->as.name.name,
                               node->as.name.length);
        return symbol >= 0 &&
               emit_ast_instruction(compiler, node, BC_LOAD_VAR, symbol,
                                    node->as.name.length);
    case AST_GROUP:
        return lower_ast_node(compiler, node->as.grouped);
    case AST_UNARY:
        return lower_ast_node(compiler, node->as.unary.operand) &&
               emit_ast_instruction(compiler, node, BC_NEGATE, 0, 1);
    case AST_BINARY:
    {
        if (!lower_ast_node(compiler, node->as.binary.left) ||
            !lower_ast_node(compiler, node->as.binary.right))
            return false;
        switch (node->as.binary.operation)
        {
        case AST_OP_ADD:
            return emit_ast_operator(compiler, node, BC_ADD, 0);
        case AST_OP_SUBTRACT:
            return emit_ast_operator(compiler, node, BC_SUB, 0);
        case AST_OP_MULTIPLY:
            return emit_ast_operator(compiler, node, BC_MUL, 0);
        case AST_OP_DIVIDE:
            return emit_ast_operator(compiler, node, BC_DIV, 0);
        case AST_OP_IN:
            return emit_ast_operator(compiler, node, BC_CONTAINS, 0);
        case AST_OP_EQUAL:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_EQUAL);
        case AST_OP_NOT_EQUAL:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_NOT_EQUAL);
        case AST_OP_LESS:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_LESS);
        case AST_OP_LESS_EQUAL:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_LESS_EQUAL);
        case AST_OP_GREATER:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_GREATER);
        case AST_OP_GREATER_EQUAL:
            return emit_ast_operator(compiler, node, BC_COMPARE, VALUE_OP_GREATER_EQUAL);
        case AST_OP_NEGATE:
            return false;
        }
        return false;
    }
    case AST_INDEX:
        if (node->as.index.collection->kind != AST_NAME)
        {
            compiler->cursor = compiler->source +
                ast_source_offset(compiler->source, node->span.line,
                                  node->span.column);
            compiler_error(compiler,
                           "AST lowering supports indexing a named collection only.");
            return false;
        }
        symbol = intern_symbol(compiler, node->as.index.collection->as.name.name,
                               node->as.index.collection->as.name.length);
        return symbol >= 0 && lower_ast_node(compiler, node->as.index.index) &&
               emit_ast_instruction(compiler, node, BC_LIST_GET, symbol, 0);
    case AST_MEMBER:
        if (node->as.member.object->kind != AST_NAME)
        {
            compiler->cursor = compiler->source +
                ast_source_offset(compiler->source, node->span.line,
                                  node->span.column);
            compiler_error(compiler,
                           "AST lowering supports a direct named-object field only.");
            return false;
        }
        {
            const AstNode *object = node->as.member.object;
            size_t field_length = object->as.name.length + 1 +
                                  node->as.member.length;
            char *field = malloc(field_length + 1);
            if (!field)
            {
                compiler_error(compiler, "Unable to allocate an expression field name.");
                return false;
            }
            memcpy(field, object->as.name.name, object->as.name.length);
            field[object->as.name.length] = '.';
            memcpy(field + object->as.name.length + 1, node->as.member.name,
                   node->as.member.length);
            field[field_length] = '\0';
            symbol = intern_symbol(compiler, field, field_length);
            free(field);
            return symbol >= 0 &&
                   emit_ast_instruction(compiler, node, BC_LOAD_FIELD, symbol, 1);
        }
    case AST_CALL:
    {
        int function = stdlib_function_id(ast_function_name(node),
                                          node->as.call.name_length);
        if (function < 0)
        {
            compiler_error(compiler, "AST lowering supports built-in calls only.");
            return false;
        }
        for (size_t i = 0; i < node->as.call.argument_count; i++)
            if (!lower_ast_node(compiler, node->as.call.arguments[i]))
                return false;
        if (!emit_ast_instruction(compiler, node, BC_STDLIB_CALL, function,
                                  node->as.call.name_length))
            return false;
        compiler->program->instructions[compiler->program->instruction_count - 1]
            .operand2 = (int)node->as.call.argument_count;
        return true;
    }
    case AST_MAP:
        if (node->as.map.count > (size_t)INT_MAX)
            return false;
        for (size_t i = 0; i < node->as.map.count; i++)
            if (!lower_ast_node(compiler, node->as.map.entries[i].key) ||
                !lower_ast_node(compiler, node->as.map.entries[i].value))
                return false;
        return emit_ast_instruction(compiler, node, BC_BUILD_MAP,
                                    (int)node->as.map.count, 1);
    case AST_SET:
        if (node->as.set.count > (size_t)INT_MAX)
            return false;
        for (size_t i = 0; i < node->as.set.count; i++)
            if (!lower_ast_node(compiler, node->as.set.items[i]))
                return false;
        return emit_ast_instruction(compiler, node, BC_BUILD_SET,
                                    (int)node->as.set.count, 1);
    }
    return false;
}

static bool compile_ast_into(BytecodeProgram *program, const char *source,
                             int source_line_offset, size_t source_column_offset,
                             bool allow_block_opener, bool emit_halt)
{
    if (!program || !program->runtime || !source)
        return false;
    RuntimeContext *runtime = program->runtime;
    size_t instruction_start = program->instruction_count;
    size_t symbol_start = program->symbol_count;
    int previous_line = runtime->current_line;
    runtime->current_line = source_line_offset + 1;
    AstNode *root = NULL;
    AstError error;
    if (!ast_parse_expression_mode(source, allow_block_opener, &root, &error))
    {
        report_diagnostic_span(runtime, "error", "E1001", error.message,
                               source_line_offset + (int)error.span.line,
                               error.span.column +
                                   (error.span.line == 1 ? source_column_offset : 0),
                               error.span.end_column > error.span.column
                                   ? error.span.end_column - error.span.column
                                   : 1);
        runtime->current_line = previous_line;
        return false;
    }

    Compiler compiler = {runtime, source, source, program, false,
                         source_column_offset, source_line_offset};
    bool success = lower_ast_node(&compiler, root);
    ast_free(root);
    if (success && emit_halt)
    {
        size_t end_offset = strlen(source);
        size_t line = 1;
        size_t column = 1;
        for (size_t i = 0; i < end_offset; i++)
        {
            if (source[i] == '\n')
            {
                line++;
                column = 1;
            }
            else
                column++;
        }
        Compiler end = compiler;
        end.runtime->current_line = end.source_line_offset + (int)line;
        end.source_column_offset = 0;
        end.cursor = source + end_offset;
        success = emit(&end, BC_HALT, 1, column);
    }
    if (!success)
    {
        for (size_t i = symbol_start; i < program->symbol_count; i++)
            free(program->symbols[i]);
        program->symbol_count = symbol_start;
        program->instruction_count = instruction_start;
    }
    else if (emit_halt)
        program->has_result = true;
    runtime->current_line = previous_line;
    return success;
}

bool bytecode_compile_ast_expression(RuntimeContext *runtime, const char *source,
                                     BytecodeProgram *program)
{
    if (!runtime_is_initialized(runtime) || !source || !program)
        return false;
    memset(program, 0, sizeof(*program));
    program->runtime = runtime;
    if (!compile_ast_into(program, source, runtime->current_line - 1, 0,
                          true, true))
    {
        bytecode_free(program);
        return false;
    }
    return true;
}

static bool emit_at(BytecodeProgram *program, BytecodeOpcode opcode, int operand,
                    int operand2, int line)
{
    int previous_line = program->runtime->current_line;
    program->runtime->current_line = line + 1;
    Compiler compiler = {program->runtime, "", "", program, false, 0, 0};
    bool success = emit(&compiler, opcode, operand, 1);
    if (success)
    {
        program->instructions[program->instruction_count - 1].operand2 = operand2;
        program->instructions[program->instruction_count - 1].span_length = 1;
    }
    program->runtime->current_line = previous_line;
    return success;
}

static bool emit_at3(BytecodeProgram *program, BytecodeOpcode opcode, int operand,
                     int operand2, int operand3, int line)
{
    if (!emit_at(program, opcode, operand, operand2, line))
        return false;
    program->instructions[program->instruction_count - 1].operand3 = operand3;
    return true;
}

static int intern_source(BytecodeProgram *program, const char *source, int line)
{
    int previous_line = program->runtime->current_line;
    program->runtime->current_line = line + 1;
    Compiler compiler = {program->runtime, source, source, program, false, 0, 0};
    int symbol = intern_symbol(&compiler, source, strlen(source));
    program->runtime->current_line = previous_line;
    return symbol;
}

static int line_brace_delta(const char *line, bool *has_open_brace)
{
    int delta = 0;
    bool quoted = false;
    bool escaped = false;
    *has_open_brace = false;
    for (const char *p = line; *p; p++)
    {
        if (!quoted && *p == '#')
            break;
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (*p == '\\' && quoted)
        {
            escaped = true;
            continue;
        }
        if (*p == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (!quoted && *p == '{')
        {
            delta++;
            *has_open_brace = true;
        }
        else if (!quoted && *p == '}')
            delta--;
    }
    return delta;
}

static bool locate_block(RuntimeContext *runtime, int header, int limit,
                         int *body_start, int *body_end, int *closing_line)
{
    int depth = 0;
    bool opened = false;
    int opening_line = -1;
    for (int line = header; line <= limit; line++)
    {
        bool has_open_brace;
        int delta = line_brace_delta(runtime->source_lines[line], &has_open_brace);
        if (line > header && opening_line < 0 && !has_open_brace)
        {
            const char *candidate = runtime->source_lines[line];
            while (*candidate == ' ' || *candidate == '\t')
                candidate++;
            if (*candidate != '\0' && *candidate != '#' && *candidate != '\r' && *candidate != '\n')
                return false;
        }
        if (has_open_brace && opening_line < 0)
        {
            opened = true;
            opening_line = line;
        }
        depth += delta;
        if (depth < 0)
            return false;
        if (opened && depth == 0)
        {
            *body_start = opening_line + 1;
            *body_end = line - 1;
            *closing_line = line;
            return true;
        }
    }
    return false;
}

static bool locate_when_block(RuntimeContext *runtime, int header, int limit,
                              int *body_start, int *body_end, int *closing_line)
{
    int depth = 0;
    bool opened = false;
    int opening_line = -1;
    for (int line = header; line <= limit; line++)
    {
        const char *source = runtime->source_lines[line];
        bool quoted = false;
        bool escaped = false;
        for (const char *cursor = source; *cursor; cursor++)
        {
            if (!quoted && *cursor == '#')
                break;
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (*cursor == '\\' && quoted)
            {
                escaped = true;
                continue;
            }
            if (*cursor == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (quoted)
                continue;

            if (*cursor == '{')
            {
                if (!opened)
                {
                    opened = true;
                    opening_line = line;
                }
                depth++;
            }
            else if (*cursor == '}')
            {
                depth--;
                if (depth < 0)
                    return false;
                if (opened && depth == 0)
                {
                    *body_start = opening_line + 1;
                    *body_end = line - 1;
                    *closing_line = line;
                    return true;
                }
            }
        }
        if (line > header && opening_line < 0)
        {
            const char *candidate = source;
            while (*candidate == ' ' || *candidate == '\t')
                candidate++;
            if (*candidate != '\0' && *candidate != '#' &&
                *candidate != '\r' && *candidate != '\n')
                return false;
        }
    }
    return false;
}

static bool patch_jump(BytecodeProgram *program, int instruction_index, size_t target)
{
    if (!program || instruction_index < 0 ||
        (size_t)instruction_index >= program->instruction_count || target > (size_t)INT_MAX)
        return false;
    program->instructions[instruction_index].operand = (int)target;
    return true;
}

static bool find_inline_otherwise(RuntimeContext *runtime, int header,
                                  int closing_line, size_t *open_brace_offset)
{
    int depth = 0;
    for (int line = header; line <= closing_line; line++)
    {
        const char *source = runtime->source_lines[line];
        bool quoted = false;
        bool escaped = false;
        for (const char *cursor = source; *cursor; cursor++)
        {
            if (!quoted && *cursor == '#')
                break;
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (*cursor == '\\' && quoted)
            {
                escaped = true;
                continue;
            }
            if (*cursor == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (quoted)
                continue;
            if (*cursor == '{')
                depth++;
            else if (*cursor == '}')
            {
                depth--;
                if (line != closing_line || depth != 0)
                    continue;

                const char *otherwise = cursor + 1;
                while (*otherwise == ' ' || *otherwise == '\t')
                    otherwise++;
                if (strncmp(otherwise, "otherwise", 9) != 0 ||
                    (isalnum((unsigned char)otherwise[9]) || otherwise[9] == '_'))
                    return false;
                otherwise += 9;
                while (*otherwise == ' ' || *otherwise == '\t')
                    otherwise++;
                if (*otherwise != '{')
                    return false;
                *open_brace_offset = (size_t)(otherwise - source);
                return true;
            }
        }
    }
    return false;
}

static bool locate_inline_otherwise_block(RuntimeContext *runtime, int header,
                                          size_t open_brace_offset, int limit,
                                          int *body_start, int *body_end, int *closing_line)
{
    const char *source = runtime->source_lines[header];
    size_t source_length = strlen(source);
    if (open_brace_offset >= source_length || source[open_brace_offset] != '{')
        return false;

    char suffix[MAX_LINE_LENGTH];
    size_t suffix_length = source_length - open_brace_offset - 1;
    if (suffix_length >= sizeof(suffix))
        return false;
    memcpy(suffix, source + open_brace_offset + 1, suffix_length);
    suffix[suffix_length] = '\0';
    int depth = 1 + line_brace_delta(suffix, &(bool){false});
    if (depth <= 0)
    {
        *body_start = header + 1;
        *body_end = header;
        *closing_line = header;
        return true;
    }

    for (int line = header + 1; line <= limit; line++)
    {
        depth += line_brace_delta(runtime->source_lines[line], &(bool){false});
        if (depth < 0)
            return false;
        if (depth == 0)
        {
            *body_start = header + 1;
            *body_end = line - 1;
            *closing_line = line;
            return true;
        }
    }
    return false;
}

static bool register_actions(RuntimeContext *runtime, int first_line, int last_line)
{
    clear_functions(runtime);
    int depth = 0;
    for (int line_index = first_line; line_index <= last_line; line_index++)
    {
        char line[MAX_LINE_LENGTH];
        strncpy(line, runtime->source_lines[line_index], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        char *candidate = line;
        while (*candidate == ' ' || *candidate == '\t')
            candidate++;

        ClassDefinition class_definition;
        if (depth == 0 && parse_class_header(candidate, &class_definition))
        {
            int class_index = find_class_index(runtime, class_definition.name);
            int body_start, body_end, close_line;
            if (class_index < 0 ||
                !locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
                return false;
            int class_depth = 0;
            for (int member_line = body_start; member_line <= body_end; member_line++)
            {
                char member[MAX_LINE_LENGTH];
                strncpy(member, runtime->source_lines[member_line], sizeof(member) - 1);
                member[sizeof(member) - 1] = '\0';
                member[strcspn(member, "\r\n")] = '\0';
                char *member_command = member;
                while (*member_command == ' ' || *member_command == '\t')
                    member_command++;
                Function method;
                if (class_depth == 0 && parse_action_header(member_command, &method))
                {
                    int method_start, method_end, method_close;
                    if (!locate_block(runtime, member_line, close_line - 1, &method_start,
                                      &method_end, &method_close))
                        return false;
                    strcpy(method.owner_class, class_definition.name);
                    method.header_line = member_line;
                    method.start_line = method_start;
                    method.end_line = method_close;
                    for (size_t parameter = 0; parameter < method.parameter_count; parameter++)
                    {
                        if (strcmp(method.parameters[parameter], "this") == 0)
                        {
                            report_diagnostic(runtime, "error", "E1312", "'this' is reserved and cannot be a method parameter",
                                              member_line + 1, 1);
                            return false;
                        }
                    }
                    if (!add_function(runtime, &method))
                    {
                        report_diagnostic(runtime, "error", "E1304", "duplicate method name or allocation failure",
                                          member_line + 1, 1);
                        return false;
                    }
                    member_line = method_close;
                    continue;
                }
                if (class_depth == 0 && match_command(member_command, "action"))
                {
                    report_diagnostic(runtime, "error", "E1310", "invalid method signature; expected action name(parameters) {",
                                      member_line + 1, 1);
                    return false;
                }
                if (class_depth == 0 && *member_command != '\0' &&
                    *member_command != '#' && strncmp(member_command, "field", 5) != 0)
                {
                    report_diagnostic(runtime, "error", "E1311", "class bodies may contain only fields and actions",
                                      member_line + 1, 1);
                    return false;
                }
                class_depth += line_brace_delta(runtime->source_lines[member_line], &(bool){false});
            }
            line_index = close_line;
            continue;
        }

        Function function;
        if (depth == 0 && parse_action_header(candidate, &function))
        {
            int body_start;
            int body_end;
            int close_line;
            if (!locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
            {
                report_diagnostic(runtime, "error", "E1201", "expected a balanced action body",
                                  line_index + 1, 1);
                return false;
            }
            function.header_line = line_index;
            function.start_line = body_start;
            function.end_line = close_line;
            if (!add_function(runtime, &function))
            {
                report_diagnostic(runtime, "error", "E1202", "duplicate action name or allocation failure",
                                  line_index + 1, 1);
                return false;
            }
            line_index = close_line;
            continue;
        }
        depth += line_brace_delta(runtime->source_lines[line_index], &(bool){false});
        if (depth < 0)
        {
            report_diagnostic(runtime, "error", "E1203", "unexpected closing brace",
                              line_index + 1, 1);
            return false;
        }
    }
    if (depth != 0)
    {
        report_diagnostic(runtime, "error", "E1204", "unclosed source block",
                          last_line + 1, 1);
        return false;
    }
    for (size_t i = 0; i < runtime->func_count; i++)
    {
        if (runtime->funcs[i].owner_class[0] == '\0')
            continue;
        int class_index = find_class_index(runtime, runtime->funcs[i].owner_class);
        if (class_index < 0)
            return false;
        int overridden = find_class_method(runtime,
                                           runtime->classes[class_index].parent_index,
                                           runtime->funcs[i].name);
        if (overridden >= 0 &&
            runtime->funcs[overridden].parameter_count !=
                runtime->funcs[i].parameter_count)
        {
            report_diagnostic(runtime, "error", "E1313", "overridden method must keep its parameter count",
                              runtime->funcs[i].header_line + 1, 1);
            return false;
        }
    }
    return true;
}

static bool compile_range(BytecodeProgram *program, int first_line, int last_line,
                          unsigned int nesting, bool in_action);

static const char *skip_call_space(const char *source)
{
    while (*source == ' ' || *source == '\t')
        source++;
    return source;
}

static bool read_call_identifier(const char **source, char destination[64])
{
    const char *start = *source;
    if (!(isalpha((unsigned char)*start) || *start == '_'))
        return false;
    const char *end = start + 1;
    while (isalnum((unsigned char)*end) || *end == '_')
        end++;
    size_t length = (size_t)(end - start);
    if (length >= 64)
        return false;
    memcpy(destination, start, length);
    destination[length] = '\0';
    *source = end;
    return true;
}

static bool parse_class_field(const char *line, ClassField *field)
{
    const char *cursor = skip_call_space(line);
    if (strncmp(cursor, "field", 5) != 0 ||
        (isalnum((unsigned char)cursor[5]) || cursor[5] == '_'))
        return false;
    cursor = skip_call_space(cursor + 5);
    if (!read_call_identifier(&cursor, field->name) || strcmp(field->name, "this") == 0)
        return false;
    cursor = skip_call_space(cursor);
    if (*cursor++ != '=')
        return false;
    cursor = skip_call_space(cursor);
    const char *comment_end = cursor;
    bool quoted = false;
    while (*comment_end && *comment_end != '\r' && *comment_end != '\n')
    {
        if (*comment_end == '"')
            quoted = !quoted;
        if (!quoted && *comment_end == '#')
            break;
        comment_end++;
    }
    size_t length = (size_t)(comment_end - cursor);
    while (length > 0 && isspace((unsigned char)cursor[length - 1]))
        length--;
    if (length == 0)
        return false;
    if (cursor[0] == '"' && length >= 2 && cursor[length - 1] == '"')
    {
        size_t string_length = length - 2;
        if (string_length >= sizeof(field->string_value))
            return false;
        memcpy(field->string_value, cursor + 1, string_length);
        field->string_value[string_length] = '\0';
        field->is_string = 1;
        return true;
    }

    char number[64];
    if (length >= sizeof(number))
        return false;
    memcpy(number, cursor, length);
    number[length] = '\0';
    errno = 0;
    char *end = NULL;
    long value = strtol(number, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value < INT_MIN || value > INT_MAX)
        return false;
    field->integer_value = (int)value;
    return true;
}

static bool validate_class_inheritance(RuntimeContext *runtime, int class_index,
                                       unsigned char *states)
{
    if (states[class_index] == 1)
        return false;
    if (states[class_index] == 2)
        return true;
    states[class_index] = 1;
    ClassDefinition *definition = &runtime->classes[class_index];
    if (definition->parent_name[0] != '\0')
    {
        definition->parent_index = find_class_index(runtime, definition->parent_name);
        if (definition->parent_index < 0 ||
            !validate_class_inheritance(runtime, definition->parent_index, states))
            return false;
        ClassDefinition *parent = &runtime->classes[definition->parent_index];
        size_t inherited_count = parent->field_count;
        for (int ancestor = parent->parent_index; ancestor >= 0;
             ancestor = runtime->classes[ancestor].parent_index)
        {
            inherited_count += runtime->classes[ancestor].field_count;
            for (size_t i = 0; i < definition->field_count; i++)
                for (size_t j = 0; j < runtime->classes[ancestor].field_count; j++)
                    if (strcmp(definition->fields[i].name,
                               runtime->classes[ancestor].fields[j].name) == 0)
                        return false;
        }
        for (size_t i = 0; i < definition->field_count; i++)
        {
            for (size_t j = 0; j < parent->field_count; j++)
            {
                if (strcmp(definition->fields[i].name, parent->fields[j].name) == 0)
                    return false;
            }
        }
        if (definition->field_count + inherited_count > MAX_CLASS_FIELDS)
            return false;
    }
    states[class_index] = 2;
    return true;
}

static bool register_classes(RuntimeContext *runtime, int first_line, int last_line)
{
    clear_classes(runtime);
    int depth = 0;
    for (int line_index = first_line; line_index <= last_line; line_index++)
    {
        char line[MAX_LINE_LENGTH];
        strncpy(line, runtime->source_lines[line_index], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        char *candidate = line;
        while (*candidate == ' ' || *candidate == '\t')
            candidate++;

        ClassDefinition definition;
        if (depth == 0 && parse_class_header(candidate, &definition))
        {
            if (runtime->class_count >= MAX_CLASSES ||
                find_class_index(runtime, definition.name) >= 0 ||
                !locate_block(runtime, line_index, last_line, &definition.body_start,
                              &definition.body_end, &definition.closing_line))
            {
                report_diagnostic(runtime, "error", "E1301", "invalid, duplicate, or unclosed class definition",
                                  line_index + 1, 1);
                return false;
            }
            definition.header_line = line_index;
            runtime->classes[runtime->class_count++] = definition;
            line_index = definition.closing_line;
            continue;
        }

        Function action;
        if (depth == 0 && parse_action_header(candidate, &action))
        {
            int body_start, body_end, close_line;
            if (!locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
            {
                report_diagnostic(runtime, "error", "E1201", "expected a balanced action body",
                                  line_index + 1, 1);
                return false;
            }
            line_index = close_line;
            continue;
        }
        depth += line_brace_delta(runtime->source_lines[line_index], &(bool){false});
        if (depth < 0)
        {
            report_diagnostic(runtime, "error", "E1203", "unexpected closing brace",
                              line_index + 1, 1);
            return false;
        }
    }
    if (depth != 0)
    {
        report_diagnostic(runtime, "error", "E1204", "unclosed source block",
                          last_line + 1, 1);
        return false;
    }

    for (size_t class_index = 0; class_index < runtime->class_count; class_index++)
    {
        ClassDefinition *definition = &runtime->classes[class_index];
        int depth_in_class = 0;
        for (int line_index = definition->body_start; line_index <= definition->body_end; line_index++)
        {
            char line[MAX_LINE_LENGTH];
            strncpy(line, runtime->source_lines[line_index], sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            line[strcspn(line, "\r\n")] = '\0';
            char *candidate = line;
            while (*candidate == ' ' || *candidate == '\t')
                candidate++;
            ClassField field;
            memset(&field, 0, sizeof(field));
            if (depth_in_class == 0 && strncmp(candidate, "field", 5) == 0 &&
                (isspace((unsigned char)candidate[5]) || candidate[5] == '\0'))
            {
                if (definition->field_count >= MAX_CLASS_FIELDS ||
                    !parse_class_field(candidate, &field))
                {
                    report_diagnostic(runtime, "error", "E1302", "invalid field declaration; expected field name = integer or string literal",
                                      line_index + 1, 1);
                    return false;
                }
                for (size_t i = 0; i < definition->field_count; i++)
                    if (strcmp(definition->fields[i].name, field.name) == 0)
                    {
                        report_diagnostic(runtime, "error", "E1302", "duplicate field declaration",
                                          line_index + 1, 1);
                        return false;
                    }
                definition->fields[definition->field_count++] = field;
            }
            depth_in_class += line_brace_delta(runtime->source_lines[line_index], &(bool){false});
        }
    }

    unsigned char states[MAX_CLASSES] = {0};
    for (size_t i = 0; i < runtime->class_count; i++)
    {
        if (!validate_class_inheritance(runtime, (int)i, states))
        {
            report_diagnostic(runtime, "error", "E1303", "unknown parent, duplicate inherited field, inheritance cycle, or class field limit exceeded",
                              runtime->classes[i].header_line + 1, 1);
            return false;
        }
    }
    return true;
}

static bool valid_assignment_syntax(const char *command)
{
    const char *equals = strchr(command, '=');
    if (!equals || equals == command || equals[1] == '=')
        return false;
    if (equals[-1] == '=' || equals[-1] == '<' || equals[-1] == '>' || equals[-1] == '!')
        return false;

    const char *left_end = equals;
    while (left_end > command && isspace((unsigned char)left_end[-1]))
        left_end--;
    const char *right = equals + 1;
    while (isspace((unsigned char)*right))
        right++;
    if (*right == '\0' || *right == '#' || *right == '\r' || *right == '\n')
        return false;

    const char *identifier = command;
    while (identifier < left_end && isspace((unsigned char)*identifier))
        identifier++;
    if (identifier == left_end ||
        !(isalpha((unsigned char)*identifier) || *identifier == '_'))
        return false;
    identifier++;
    while (identifier < left_end &&
           (isalnum((unsigned char)*identifier) || *identifier == '_'))
        identifier++;
    if (identifier < left_end && *identifier == '.')
    {
        identifier++;
        if (identifier == left_end ||
            !(isalpha((unsigned char)*identifier) || *identifier == '_'))
            return false;
        identifier++;
        while (identifier < left_end &&
               (isalnum((unsigned char)*identifier) || *identifier == '_'))
            identifier++;
    }
    else if (identifier < left_end && *identifier == '[')
    {
        const char *index_start = ++identifier;
        unsigned int nesting = 1;
        bool quoted = false;
        while (identifier < left_end && nesting > 0)
        {
            if (*identifier == '"')
                quoted = !quoted;
            else if (!quoted && *identifier == '[')
                nesting++;
            else if (!quoted && *identifier == ']')
                nesting--;
            identifier++;
        }
        if (nesting != 0 || index_start == identifier - 1)
            return false;
    }
    while (identifier < left_end && isspace((unsigned char)*identifier))
        identifier++;
    return identifier == left_end;
}

static bool declaration_type_from_name(const char *name, size_t length,
                                       VariableDeclaredType *type)
{
    static const struct
    {
        const char *name;
        VariableDeclaredType type;
    } types[] = {
        {"Int", VARIABLE_TYPE_INT},
        {"Float", VARIABLE_TYPE_FLOAT},
        {"Bool", VARIABLE_TYPE_BOOL},
        {"String", VARIABLE_TYPE_STRING},
        {"Null", VARIABLE_TYPE_NULL}};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    {
        if (strlen(types[i].name) == length &&
            memcmp(types[i].name, name, length) == 0)
        {
            *type = types[i].type;
            return true;
        }
    }
    return false;
}

static TypecheckType typecheck_type_for_declaration(VariableDeclaredType type)
{
    switch (type)
    {
    case VARIABLE_TYPE_INT:
        return TYPECHECK_INT;
    case VARIABLE_TYPE_FLOAT:
        return TYPECHECK_FLOAT;
    case VARIABLE_TYPE_BOOL:
        return TYPECHECK_BOOL;
    case VARIABLE_TYPE_STRING:
        return TYPECHECK_STRING;
    case VARIABLE_TYPE_NULL:
        return TYPECHECK_NULL;
    case VARIABLE_TYPE_UNDECLARED:
        break;
    }
    return TYPECHECK_UNKNOWN_TYPE;
}

static bool emit_typed_declaration(BytecodeProgram *program, char *command,
                                   int source_line, bool *handled)
{
    RuntimeContext *runtime = program->runtime;
    *handled = false;
    const char *cursor = command;
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (!(isalpha((unsigned char)*cursor) || *cursor == '_'))
        return true;
    const char *name_start = cursor++;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    const char *name_end = cursor;
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor != ':')
        return true;
    *handled = true;
    cursor++;
    while (isspace((unsigned char)*cursor))
        cursor++;
    const char *type_start = cursor;
    if (!(isalpha((unsigned char)*cursor) || *cursor == '_'))
        goto invalid_declaration;
    cursor++;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    size_t type_length = (size_t)(cursor - type_start);
    VariableDeclaredType declared_type;
    if (!declaration_type_from_name(type_start, type_length, &declared_type))
    {
        report_diagnostic(runtime, "error", "E1001",
                          "unsupported variable type; expected Int, Float, Bool, String, or Null",
                          source_line + 1, 1);
        return false;
    }
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor++ != '=')
        goto invalid_declaration;
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor == '\0' || *cursor == '#')
        goto invalid_declaration;

    size_t name_length = (size_t)(name_end - name_start);
    if (name_length >= 64)
    {
        report_diagnostic(runtime, "error", "E1001",
                          "identifier exceeds the 63-character limit",
                          source_line + 1, 1);
        return false;
    }

    AstNode *initializer = NULL;
    AstError parse_error;
    if (ast_parse_expression_mode(cursor, false, &initializer, &parse_error))
    {
        TypecheckResult checked = typecheck_expression(initializer);
        if (checked.status == TYPECHECK_ERROR ||
            (checked.status == TYPECHECK_OK &&
             checked.type != typecheck_type_for_declaration(declared_type)))
        {
            char message[160];
            if (checked.status == TYPECHECK_ERROR)
                (void)snprintf(message, sizeof(message), "%s", checked.message);
            else
                (void)snprintf(message, sizeof(message),
                               "initializer has type %s; declaration requires %s",
                               typecheck_type_name(checked.type),
                               typecheck_type_name(
                                   typecheck_type_for_declaration(declared_type)));
            const char *original = runtime->source_lines[source_line];
            const char *initializer_start = strstr(original, cursor);
            size_t column = initializer_start
                                ? (size_t)(initializer_start - original) + 1
                                : 1;
            report_diagnostic_span(runtime, "error", "E1001", message,
                                   source_line + 1,
                                   column + checked.span.column - 1,
                                   checked.span.end_column > checked.span.column
                                       ? checked.span.end_column -
                                             checked.span.column
                                       : 1);
            ast_free(initializer);
            return false;
        }
        ast_free(initializer);
    }

    char variable_name[64];
    memcpy(variable_name, name_start, name_length);
    variable_name[name_length] = '\0';
    int symbol = intern_source(program, variable_name, source_line);
    if (symbol < 0 || !emit_expression_into(program, (char *)cursor, source_line))
        return false;
    if (!emit_at(program, BC_DECLARE_VAR, symbol,
                 (int)declared_type, source_line))
        return false;
    return true;

invalid_declaration:
    report_diagnostic(runtime, "error", "E1001",
                      "invalid typed declaration; expected name: Type = expression",
                      source_line + 1, 1);
    return false;
}

static bool find_assignment_operator(char *command, char **equals)
{
    bool quoted = false;
    bool escaped = false;
    unsigned int nesting = 0;
    for (char *cursor = command; *cursor; cursor++)
    {
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (quoted && *cursor == '\\')
        {
            escaped = true;
            continue;
        }
        if (*cursor == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (*cursor == '(' || *cursor == '[' || *cursor == '{')
            nesting++;
        else if ((*cursor == ')' || *cursor == ']' || *cursor == '}') && nesting > 0)
            nesting--;
        else if (*cursor == '=' && nesting == 0 &&
                 cursor[1] != '=' &&
                 (cursor == command ||
                  (cursor[-1] != '=' && cursor[-1] != '<' &&
                   cursor[-1] != '>' && cursor[-1] != '!')))
        {
            *equals = cursor;
            return true;
        }
    }
    return false;
}

static bool emit_value_assignment(BytecodeProgram *program, char *command,
                                  int source_line, bool *handled)
{
    RuntimeContext *runtime = program->runtime;
    *handled = false;
    char *equals = NULL;
    if (!find_assignment_operator(command, &equals))
        return true;

    char *target_start = command;
    while (isspace((unsigned char)*target_start))
        target_start++;
    char *target_end = equals;
    while (target_end > target_start && isspace((unsigned char)target_end[-1]))
        target_end--;
    char *expression = equals + 1;
    while (isspace((unsigned char)*expression))
        expression++;

    bool indexed_target = memchr(target_start, '[', (size_t)(target_end - target_start)) != NULL;
    if (!indexed_target)
    {
        const char *target_cursor = target_start;
        while (target_cursor < target_end &&
               (isalnum((unsigned char)*target_cursor) || *target_cursor == '_'))
            target_cursor++;
        if (target_cursor < target_end && *target_cursor == '.')
        {
            const char *receiver_start = target_start;
            const char *receiver_end = target_cursor;
            const char *field_start = target_cursor + 1;
            const char *field_end = target_end;
            while (field_end > field_start && isspace((unsigned char)field_end[-1]))
                field_end--;
            if (receiver_end == receiver_start || field_start == field_end ||
                (size_t)(receiver_end - receiver_start) >= 64 ||
                (size_t)(field_end - field_start) >= 64)
                goto invalid_field_assignment;
            for (const char *cursor = field_start; cursor < field_end; cursor++)
                if (!(isalnum((unsigned char)*cursor) || *cursor == '_'))
                    goto invalid_field_assignment;
            char receiver[64], field[64];
            size_t receiver_length = (size_t)(receiver_end - receiver_start);
            size_t field_length = (size_t)(field_end - field_start);
            memcpy(receiver, receiver_start, receiver_length);
            receiver[receiver_length] = '\0';
            memcpy(field, field_start, field_length);
            field[field_length] = '\0';
            int receiver_symbol = intern_source(program, receiver, source_line);
            int field_symbol = intern_source(program, field, source_line);
            if (receiver_symbol < 0 || field_symbol < 0 ||
                !emit_expression_into(program, expression, source_line))
                return false;
            *handled = true;
            return emit_at3(program, BC_STORE_FIELD, receiver_symbol,
                            field_symbol, 0, source_line);
        invalid_field_assignment:
            report_diagnostic(runtime, "error", "E1001", "invalid object field assignment target",
                              source_line + 1, 1);
            *handled = true;
            return false;
        }
    }
    *handled = true;

    const char *name_start = target_start;
    if (!(isalpha((unsigned char)*name_start) || *name_start == '_'))
        return false;
    const char *name_end = name_start + 1;
    while (name_end < target_end &&
           (isalnum((unsigned char)*name_end) || *name_end == '_'))
        name_end++;
    size_t name_length = (size_t)(name_end - name_start);
    if (name_length >= 64)
    {
        report_diagnostic(runtime, "error", "E1001", "identifier exceeds the 63-character limit",
                          source_line + 1, 1);
        return false;
    }
    char variable_name[64];
    memcpy(variable_name, name_start, name_length);
    variable_name[name_length] = '\0';
    int symbol = intern_source(program, variable_name, source_line);
    if (symbol < 0)
        return false;

    int previous_line = program->runtime->current_line;
    program->runtime->current_line = source_line + 1;
    if (indexed_target)
    {
        const char *cursor = name_end;
        while (cursor < target_end && isspace((unsigned char)*cursor))
            cursor++;
        if (cursor == target_end || *cursor++ != '[')
        {
            report_diagnostic(runtime, "error", "E1001", "invalid indexed assignment target",
                              source_line + 1, 1);
            program->runtime->current_line = previous_line;
            return false;
        }
        size_t index_length = (size_t)(target_end - cursor);
        if (index_length == 0 || target_end[-1] != ']')
        {
            report_diagnostic(runtime, "error", "E1001", "expected ']' in indexed assignment target",
                              source_line + 1, 1);
            program->runtime->current_line = previous_line;
            return false;
        }
        char *index_source = malloc(index_length);
        if (!index_source)
        {
            report_diagnostic(runtime, "error", "E1001", "unable to allocate collection index expression",
                              source_line + 1, 1);
            program->runtime->current_line = previous_line;
            return false;
        }
        memcpy(index_source, cursor, index_length - 1);
        index_source[index_length - 1] = '\0';
        bool success = emit_expression_into(program, index_source, source_line);
        free(index_source);
        if (success)
            success = emit_expression_into(program, expression, source_line);
        if (success)
            success = emit_at(program, BC_MAP_SET, symbol, 0, source_line);
        program->runtime->current_line = previous_line;
        return success;
    }

    if (name_end != target_end)
    {
        report_diagnostic(runtime, "error", "E1001", "collection assignment target must be a variable name",
                          source_line + 1, 1);
        program->runtime->current_line = previous_line;
        return false;
    }
    bool success = emit_expression_into(program, expression, source_line) &&
                   emit_at(program, BC_STORE_VAR, symbol, 0, source_line);
    program->runtime->current_line = previous_line;
    return success;
}

static bool emit_input_statement(BytecodeProgram *program, char *command, int source_line)
{
    RuntimeContext *runtime = program->runtime;
    const char *cursor = skip_cmd(command, "input");
    char encoded_names[MAX_LINE_LENGTH];
    size_t encoded_length = 0;
    size_t name_count = 0;

    while (*cursor)
    {
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (!*cursor)
            break;
        if (!(isalpha((unsigned char)*cursor) || *cursor == '_'))
            goto invalid_input;

        const char *name_start = cursor++;
        while (isalnum((unsigned char)*cursor) || *cursor == '_')
            cursor++;
        size_t name_length = (size_t)(cursor - name_start);
        if (name_length >= 64 ||
            (*cursor && !isspace((unsigned char)*cursor)) ||
            encoded_length + name_length + (name_count != 0) >= sizeof(encoded_names))
            goto invalid_input;

        if (name_count != 0)
            encoded_names[encoded_length++] = ' ';
        memcpy(encoded_names + encoded_length, name_start, name_length);
        encoded_length += name_length;
        name_count++;
    }
    if (name_count == 0)
        goto invalid_input;

    {
        int symbol = intern_source(program, encoded_names, source_line);
        if (symbol < 0 ||
            !emit_at(program, BC_INPUT, symbol, 0, source_line))
            return false;
        program->instructions[program->instruction_count - 1].operand2 = (int)name_count;
    }
    return true;

invalid_input:
    report_diagnostic(runtime, "error", "E1001",
                      "expected 'input name [name ...]' with valid variable names",
                      source_line + 1, 1);
    return false;
}

static bool read_simple_argument(const char **source, char destination[128])
{
    while (**source == ' ' || **source == '\t')
        (*source)++;
    const char *start = *source;
    while (**source && !isspace((unsigned char)**source) &&
           **source != '#' && **source != '\r' && **source != '\n')
        (*source)++;
    size_t length = (size_t)(*source - start);
    if (length == 0 || length >= 128)
        return false;
    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static bool command_has_only_space_or_comment(const char *source)
{
    while (*source == ' ' || *source == '\t')
        source++;
    return *source == '\0' || *source == '#' || *source == '\r' || *source == '\n';
}

static bool emit_collection_command(BytecodeProgram *program, char *command,
                                    int source_line, bool *handled)
{
    RuntimeContext *runtime = program->runtime;
    *handled = true;
    bool is_matrix = match_command(command, "matrix");
    bool is_matrix_set = match_command(command, "matrix_set");
    bool is_matrix_get = match_command(command, "matrix_get");
    bool is_queue = match_command(command, "queue");
    bool is_enqueue = match_command(command, "enqueue");
    bool is_dequeue = match_command(command, "dequeue");
    bool is_peek = match_command(command, "peek");
    bool is_len = match_command(command, "len");
    const char *cursor = skip_cmd(command,
                                  is_matrix       ? "matrix"
                                  : is_matrix_set ? "matrix_set"
                                  : is_matrix_get ? "matrix_get"
                                  : is_queue      ? "queue"
                                  : is_enqueue    ? "enqueue"
                                  : is_dequeue    ? "dequeue"
                                  : is_peek       ? "peek"
                                                  : "len");
    char collection_name[128];
    char arguments[3][128];
    size_t argument_count = is_matrix       ? 2
                            : is_matrix_set ? 3
                            : is_matrix_get ? 2
                            : is_enqueue    ? 1
                                                     : 0;
    if (!read_simple_argument(&cursor, collection_name))
        goto invalid_command;
    for (size_t i = 0; i < argument_count; i++)
        if (!read_simple_argument(&cursor, arguments[i]))
            goto invalid_command;

    if (!(isalpha((unsigned char)collection_name[0]) || collection_name[0] == '_'))
        goto invalid_command;
    for (const char *name_cursor = collection_name + 1; *name_cursor; name_cursor++)
        if (!(isalnum((unsigned char)*name_cursor) || *name_cursor == '_'))
            goto invalid_command;
    if (strlen(collection_name) >= 64)
        goto invalid_command;
    int collection_symbol = intern_source(program, collection_name, source_line);
    if (collection_symbol < 0)
        return false;

    int target_symbol = -1;
    if (is_matrix_get || is_dequeue || is_peek || is_len)
    {
        char target_name[128];
        if (!read_simple_argument(&cursor, target_name) ||
            !command_has_only_space_or_comment(cursor) ||
            !(isalpha((unsigned char)target_name[0]) || target_name[0] == '_'))
            goto invalid_command;
        for (const char *target_cursor = target_name + 1; *target_cursor; target_cursor++)
            if (!(isalnum((unsigned char)*target_cursor) || *target_cursor == '_'))
                goto invalid_command;
        if (strlen(target_name) >= 64)
            goto invalid_command;
        target_symbol = intern_source(program, target_name, source_line);
        if (target_symbol < 0)
            return false;
    }
    if (!command_has_only_space_or_comment(cursor))
        goto invalid_command;

    if (is_matrix)
    {
        if (!emit_expression_into(program, arguments[0], source_line) ||
            !emit_expression_into(program, arguments[1], source_line))
            return false;
        return emit_at(program, BC_MATRIX_CREATE, collection_symbol, 0, source_line);
    }
    if (is_matrix_set)
    {
        if (!emit_expression_into(program, arguments[0], source_line) ||
            !emit_expression_into(program, arguments[1], source_line) ||
            !emit_expression_into(program, arguments[2], source_line))
            return false;
        return emit_at(program, BC_MATRIX_SET, collection_symbol, 0, source_line);
    }
    if (is_matrix_get)
    {
        if (!emit_expression_into(program, arguments[0], source_line) ||
            !emit_expression_into(program, arguments[1], source_line))
            return false;
        return emit_at(program, BC_MATRIX_GET, collection_symbol, target_symbol,
                       source_line);
    }
    if (is_queue)
        return emit_at(program, BC_QUEUE_CREATE, collection_symbol, 0, source_line);
    if (is_enqueue)
    {
        if (!emit_expression_into(program, arguments[0], source_line))
            return false;
        return emit_at(program, BC_QUEUE_ENQUEUE, collection_symbol, 0, source_line);
    }
    if (is_dequeue)
        return emit_at(program, BC_QUEUE_DEQUEUE, collection_symbol, target_symbol,
                       source_line);
    if (is_peek)
        return emit_at(program, BC_QUEUE_PEEK, collection_symbol, target_symbol,
                       source_line);
    return emit_at(program, BC_COLLECTION_LEN, collection_symbol, target_symbol,
                   source_line);

invalid_command:
    report_diagnostic(runtime, "error", "E1001", "invalid matrix, queue, or len command arguments",
                      source_line + 1, 1);
    return false;
}

static bool read_path_argument(const char **source, char destination[260])
{
    while (**source == ' ' || **source == '\t')
        (*source)++;
    bool quoted = **source == '"';
    if (quoted)
        (*source)++;
    const char *start = *source;
    while (**source && **source != '\r' && **source != '\n' &&
           (quoted ? **source != '"' : !isspace((unsigned char)**source)))
        (*source)++;
    size_t length = (size_t)(*source - start);
    if (length == 0 || length >= 260 || (quoted && **source != '"'))
        return false;
    memcpy(destination, start, length);
    destination[length] = '\0';
    if (quoted)
        (*source)++;
    return true;
}

static bool valid_variable_name(const char *name)
{
    if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_') ||
        strlen(name) >= 64)
        return false;
    for (const char *cursor = name + 1; *cursor; cursor++)
        if (!(isalnum((unsigned char)*cursor) || *cursor == '_'))
            return false;
    return true;
}

static bool emit_managed_statement(BytecodeProgram *program, char *command,
                                   int source_line, bool *handled)
{
    RuntimeContext *runtime = program->runtime;
    *handled = false;
    bool is_list = match_command(command, "list");
    bool is_pile = match_command(command, "pile");
    bool is_push = match_command(command, "push");
    bool is_pop = match_command(command, "pop");
    bool is_save = match_command(command, "save");
    bool is_load = match_command(command, "load");
    bool is_file_display = match_command(command, "file_display");
    bool is_file_read = match_command(command, "file_read");
    bool is_file_write = match_command(command, "file_write");
    if (!(is_list || is_pile || is_push || is_pop || is_save || is_load ||
          is_file_display || is_file_read || is_file_write))
        return true;
    *handled = true;

    const char *keyword = is_list ? "list" : is_pile ? "pile" :
                          is_push ? "push" : is_pop ? "pop" :
                          is_save ? "save" : is_load ? "load" :
                          is_file_display ? "file_display" :
                          is_file_read ? "file_read" : "file_write";
    const char *cursor = skip_cmd(command, keyword);
    char first[128];
    if (is_list || is_pile || is_push || is_pop)
    {
        if (!read_simple_argument(&cursor, first) || !valid_variable_name(first))
            goto invalid_statement;
        int collection_symbol = intern_source(program, first, source_line);
        if (collection_symbol < 0)
            return false;
        if (is_list || is_pile || is_pop)
        {
            if (!command_has_only_space_or_comment(cursor))
                goto invalid_statement;
            BytecodeOpcode opcode = is_list ? BC_LIST_CREATE :
                                    is_pile ? BC_PILE_CREATE : BC_COLLECTION_POP;
            return emit_at(program, opcode, collection_symbol, 0, source_line);
        }

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '\0' || *cursor == '#' ||
            !emit_expression_into(program, cursor, source_line))
            goto invalid_statement;
        if (!emit_at(program, BC_COLLECTION_PUSH, collection_symbol, 0, source_line))
            return false;
        return true;
    }

    if (is_save || is_load)
    {
        char collection_name[128];
        char filename[260];
        if (!read_simple_argument(&cursor, collection_name) ||
            !valid_variable_name(collection_name) ||
            !read_path_argument(&cursor, filename) ||
            !command_has_only_space_or_comment(cursor))
            goto invalid_statement;
        int collection_symbol = intern_source(program, collection_name, source_line);
        int path_symbol = intern_source(program, filename, source_line);
        if (collection_symbol < 0 || path_symbol < 0)
            return false;
        return emit_at3(program, is_save ? BC_COLLECTION_SAVE : BC_COLLECTION_LOAD,
                        collection_symbol, path_symbol, 0, source_line);
    }

    char filename[260];
    if (!read_path_argument(&cursor, filename))
        goto invalid_statement;
    int path_symbol = intern_source(program, filename, source_line);
    if (path_symbol < 0)
        return false;
    if (is_file_display)
    {
        if (!command_has_only_space_or_comment(cursor))
            goto invalid_statement;
        return emit_at(program, BC_FILE_DISPLAY, path_symbol, 0, source_line);
    }

    char target_name[128];
    if (is_file_read)
    {
        if (!read_simple_argument(&cursor, target_name) ||
            !valid_variable_name(target_name) ||
            !command_has_only_space_or_comment(cursor))
            goto invalid_statement;
        int target_symbol = intern_source(program, target_name, source_line);
        if (target_symbol < 0)
            return false;
        return emit_at(program, BC_FILE_READ, path_symbol, target_symbol, source_line);
    }

    char source_name[128];
    if (!read_simple_argument(&cursor, source_name) ||
        !valid_variable_name(source_name))
        goto invalid_statement;
    bool append = false;
    char mode[128];
    if (!command_has_only_space_or_comment(cursor))
    {
        if (!read_simple_argument(&cursor, mode) ||
            (strcmp(mode, "append") != 0 && strcmp(mode, "overwrite") != 0) ||
            !command_has_only_space_or_comment(cursor))
            goto invalid_statement;
        append = strcmp(mode, "append") == 0;
    }
    int source_symbol = intern_source(program, source_name, source_line);
    if (source_symbol < 0)
        return false;
    return emit_at3(program, BC_FILE_WRITE, path_symbol, source_symbol,
                    append ? 1 : 0, source_line);

invalid_statement:
    report_diagnostic(runtime, "error", "E1001",
                      "invalid list, pile, save/load, or file command arguments",
                      source_line + 1, 1);
    return false;
}

static bool display_token_is_special_name(const char *token)
{
    if (!(isalpha((unsigned char)token[0]) || token[0] == '_'))
        return false;
    for (const char *cursor = token + 1; *cursor; cursor++)
    {
        if (isalnum((unsigned char)*cursor) || *cursor == '_' ||
            *cursor == '[' || *cursor == ']' || *cursor == '.')
            continue;
        return false;
    }
    return true;
}

static bool expression_contains_membership_operator(const char *source)
{
    bool quoted = false;
    bool escaped = false;
    for (const char *cursor = source; *cursor; cursor++)
    {
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (quoted && *cursor == '\\')
        {
            escaped = true;
            continue;
        }
        if (*cursor == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (!quoted && cursor[0] == 'i' && cursor[1] == 'n' &&
            (cursor == source ||
             !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            !(isalnum((unsigned char)cursor[2]) || cursor[2] == '_'))
            return true;
    }
    return false;
}

static bool emit_display_statement(BytecodeProgram *program, const char *source,
                                   int source_line)
{
    RuntimeContext *runtime = program->runtime;
    const char *cursor = source;
    int previous_line = program->runtime->current_line;
    program->runtime->current_line = source_line + 1;
    if (expression_contains_membership_operator(source))
    {
        bool success = emit_expression_into(program, source, source_line) &&
                       emit_at(program, BC_DISPLAY_VALUE, -1, 0, source_line) &&
                       emit_at(program, BC_DISPLAY_NEWLINE, 0, 0, source_line);
        program->runtime->current_line = previous_line;
        return success;
    }
    while (true)
    {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '\0' || *cursor == '#' || *cursor == '\r' || *cursor == '\n')
            break;

        const char *segment_start = cursor;
        bool quoted = false;
        bool escaped = false;
        unsigned int parentheses = 0;
        unsigned int brackets = 0;
        unsigned int braces = 0;
        while (*cursor)
        {
            if (escaped)
            {
                escaped = false;
                cursor++;
                continue;
            }
            if (quoted && *cursor == '\\')
            {
                escaped = true;
                cursor++;
                continue;
            }
            if (*cursor == '"')
            {
                quoted = !quoted;
                cursor++;
                continue;
            }
            if (!quoted)
            {
                if (*cursor == '#' || *cursor == '\r' || *cursor == '\n')
                    break;
                if (*cursor == ' ' || *cursor == '\t')
                {
                    if (parentheses == 0 && brackets == 0 && braces == 0)
                        break;
                    cursor++;
                    continue;
                }
                if (*cursor == '(')
                    parentheses++;
                else if (*cursor == ')' && parentheses > 0)
                    parentheses--;
                else if (*cursor == '[')
                    brackets++;
                else if (*cursor == ']' && brackets > 0)
                    brackets--;
                else if (*cursor == '{')
                    braces++;
                else if (*cursor == '}' && braces > 0)
                    braces--;
            }
            cursor++;
        }
        size_t segment_length = (size_t)(cursor - segment_start);
        if (segment_length == 0)
        {
            report_diagnostic(runtime, "error", "E1001", "empty display expression",
                              source_line + 1, 1);
            program->runtime->current_line = previous_line;
            return false;
        }
        char *segment = malloc(segment_length + 1);
        if (!segment)
        {
            report_diagnostic(runtime, "error", "E1001", "unable to allocate display expression",
                              source_line + 1, 1);
            program->runtime->current_line = previous_line;
            return false;
        }
        memcpy(segment, segment_start, segment_length);
        segment[segment_length] = '\0';

        bool success;
        if (strchr(segment, '.') && display_token_is_special_name(segment))
        {
            int symbol = intern_source(program, segment, source_line);
            success = symbol >= 0 &&
                      emit_at(program, BC_DISPLAY_SPECIAL, symbol, 0, source_line);
        }
        else
        {
            success = emit_expression_into(program, segment, source_line);
            int special_symbol = -1;
            if (success && display_token_is_special_name(segment))
            {
                special_symbol = intern_source(program, segment, source_line);
                if (special_symbol < 0)
                    success = false;
            }
            if (success)
                success = emit_at(program, BC_DISPLAY_VALUE, special_symbol, 0, source_line);
        }
        free(segment);
        if (!success)
        {
            program->runtime->current_line = previous_line;
            return false;
        }
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
    }
    bool success = emit_at(program, BC_DISPLAY_NEWLINE, 0, 0, source_line);
    program->runtime->current_line = previous_line;
    return success;
}

static bool looks_like_action_invocation(const char *command)
{
    const char *cursor = skip_call_space(command);
    if (!(isalpha((unsigned char)*cursor) || *cursor == '_'))
        return false;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    return *skip_call_space(cursor) == '(';
}

static bool emit_action_invocation(BytecodeProgram *program, char *command, int source_line)
{
    RuntimeContext *runtime = program->runtime;
    char name[64];
    const char *name_cursor = command;
    while (*name_cursor == ' ' || *name_cursor == '\t')
        name_cursor++;
    if (!read_call_identifier(&name_cursor, name))
    {
        report_diagnostic(runtime, "error", "E1205", "expected an action name",
                          source_line + 1, 1);
        return false;
    }
    char *cursor = (char *)skip_call_space(name_cursor);
    if (*cursor != '(')
    {
        report_diagnostic(runtime, "error", "E1206", "expected '(' after action name",
                          source_line + 1, 1);
        return false;
    }

    int function_index = find_function_index(runtime, name);
    if (function_index < 0)
    {
        report_diagnostic(runtime, "error", "E1207", "called action is not defined",
                          source_line + 1, 1);
        return false;
    }

    char arguments[MAX_ACTION_PARAMETERS][MAX_LINE_LENGTH];
    size_t argument_count = 0;
    cursor++;
    cursor = (char *)skip_call_space(cursor);
    if (*cursor != ')')
    {
        while (true)
        {
            if (argument_count >= MAX_ACTION_PARAMETERS)
            {
                report_diagnostic(runtime, "error", "E1208", "too many action arguments",
                                  source_line + 1, 1);
                return false;
            }
            const char *start = cursor;
            int parentheses = 0;
            int brackets = 0;
            bool quoted = false;
            bool escaped = false;
            while (*cursor)
            {
                if (escaped)
                {
                    escaped = false;
                    cursor++;
                    continue;
                }
                if (*cursor == '\\' && quoted)
                {
                    escaped = true;
                    cursor++;
                    continue;
                }
                if (*cursor == '"')
                    quoted = !quoted;
                else if (!quoted && *cursor == '(')
                    parentheses++;
                else if (!quoted && *cursor == ')')
                {
                    if (parentheses == 0 && brackets == 0)
                        break;
                    parentheses--;
                }
                else if (!quoted && *cursor == '[')
                    brackets++;
                else if (!quoted && *cursor == ']')
                    brackets--;
                else if (!quoted && parentheses == 0 && brackets == 0 && *cursor == ',')
                    break;
                cursor++;
            }
            const char *end = cursor;
            while (start < end && isspace((unsigned char)*start))
                start++;
            while (end > start && isspace((unsigned char)end[-1]))
                end--;
            size_t length = (size_t)(end - start);
            if (length == 0 || length >= sizeof(arguments[argument_count]))
            {
                report_diagnostic(runtime, "error", "E1209", "invalid or oversized action argument",
                                  source_line + 1, 1);
                return false;
            }
            memcpy(arguments[argument_count], start, length);
            arguments[argument_count++][length] = '\0';
            if (*cursor == ')')
                break;
            if (*cursor != ',')
            {
                report_diagnostic(runtime, "error", "E1210", "expected ',' or ')' in action call",
                                  source_line + 1, 1);
                return false;
            }
            cursor++;
        }
    }
    if (*cursor++ != ')')
    {
        report_diagnostic(runtime, "error", "E1210", "expected ')' after action arguments",
                          source_line + 1, 1);
        return false;
    }
    cursor = (char *)skip_call_space(cursor);
    if (*cursor != '\0' && *cursor != '#' && *cursor != '\r' && *cursor != '\n')
    {
        report_diagnostic(runtime, "error", "E1211", "unexpected text after action call",
                          source_line + 1, 1);
        return false;
    }

    Function *function = &runtime->funcs[function_index];
    if (argument_count != function->parameter_count)
    {
        report_diagnostic(runtime, "error", "E1212", "action argument count does not match its signature",
                          source_line + 1, 1);
        return false;
    }
    for (size_t i = 0; i < argument_count; i++)
    {
        if (!emit_expression_into(program, arguments[i], source_line))
            return false;
    }
    if (program->instruction_count > (size_t)INT_MAX)
        return false;
    return emit_at(program, BC_CALL, function_index, (int)argument_count, source_line);
}

static bool parse_call_arguments(char *cursor, char arguments[MAX_ACTION_PARAMETERS][MAX_LINE_LENGTH],
                                 size_t *argument_count, int source_line,
                                 RuntimeContext *runtime)
{
    *argument_count = 0;
    cursor = (char *)skip_call_space(cursor);
    if (*cursor != '(')
        return false;
    cursor = (char *)skip_call_space(cursor + 1);
    if (*cursor != ')')
    {
        while (true)
        {
            if (*argument_count >= MAX_ACTION_PARAMETERS)
            {
                report_diagnostic(runtime, "error", "E1208", "too many call arguments", source_line + 1, 1);
                return false;
            }
            const char *start = cursor;
            int parentheses = 0, brackets = 0;
            bool quoted = false, escaped = false;
            while (*cursor)
            {
                if (escaped)
                {
                    escaped = false;
                    cursor++;
                    continue;
                }
                if (*cursor == '\\' && quoted)
                {
                    escaped = true;
                    cursor++;
                    continue;
                }
                if (*cursor == '"')
                    quoted = !quoted;
                else if (!quoted && *cursor == '(')
                    parentheses++;
                else if (!quoted && *cursor == ')')
                {
                    if (parentheses == 0 && brackets == 0)
                        break;
                    parentheses--;
                }
                else if (!quoted && *cursor == '[')
                    brackets++;
                else if (!quoted && *cursor == ']')
                    brackets--;
                else if (!quoted && parentheses == 0 && brackets == 0 && *cursor == ',')
                    break;
                cursor++;
            }
            const char *end = cursor;
            while (start < end && isspace((unsigned char)*start))
                start++;
            while (end > start && isspace((unsigned char)end[-1]))
                end--;
            size_t length = (size_t)(end - start);
            if (length == 0 || length >= MAX_LINE_LENGTH)
            {
                report_diagnostic(runtime, "error", "E1209", "invalid or oversized call argument",
                                  source_line + 1, 1);
                return false;
            }
            memcpy(arguments[*argument_count], start, length);
            arguments[(*argument_count)++][length] = '\0';
            if (*cursor == ')')
                break;
            if (*cursor != ',')
            {
                report_diagnostic(runtime, "error", "E1210", "expected ',' or ')' in call",
                                  source_line + 1, 1);
                return false;
            }
            cursor = (char *)skip_call_space(cursor + 1);
        }
    }
    if (*cursor++ != ')')
        return false;
    cursor = (char *)skip_call_space(cursor);
    if (*cursor != '\0' && *cursor != '#' && *cursor != '\r' && *cursor != '\n')
    {
        report_diagnostic(runtime, "error", "E1211", "unexpected text after call",
                          source_line + 1, 1);
        return false;
    }
    return true;
}

static bool looks_like_method_invocation(const char *command)
{
    const char *cursor = skip_call_space(command);
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    if (*cursor++ != '.')
        return false;
    if (!(isalpha((unsigned char)*cursor) || *cursor == '_'))
        return false;
    while (isalnum((unsigned char)*cursor) || *cursor == '_')
        cursor++;
    return *skip_call_space(cursor) == '(';
}

static bool parse_catch_header(const char *line, char name[64])
{
    const char *cursor = skip_call_space(line);
    if (strncmp(cursor, "catch", 5) != 0 ||
        (isalnum((unsigned char)cursor[5]) || cursor[5] == '_'))
        return false;
    cursor = skip_call_space(cursor + 5);
    if (!read_call_identifier(&cursor, name))
        return false;
    cursor = skip_call_space(cursor);
    return *cursor == '{' || *cursor == '\0' || *cursor == '#' ||
           *cursor == '\r' || *cursor == '\n';
}

static bool emit_method_invocation(BytecodeProgram *program, char *command, int source_line)
{
    RuntimeContext *runtime = program->runtime;
    char receiver[64], method[64];
    const char *cursor = skip_call_space(command);
    if (!read_call_identifier(&cursor, receiver) || *cursor++ != '.' ||
        !read_call_identifier(&cursor, method))
        return false;
    char arguments[MAX_ACTION_PARAMETERS][MAX_LINE_LENGTH];
    size_t argument_count;
    if (!parse_call_arguments((char *)cursor, arguments, &argument_count, source_line,
                              runtime))
        return false;
    for (size_t i = 0; i < argument_count; i++)
    {
        if (!emit_expression_into(program, arguments[i], source_line))
            return false;
    }
    int receiver_symbol = intern_source(program, receiver, source_line);
    int method_symbol = intern_source(program, method, source_line);
    if (receiver_symbol < 0 || method_symbol < 0 ||
        program->instruction_count > (size_t)INT_MAX)
        return false;
    return emit_at3(program, BC_METHOD_CALL, receiver_symbol, method_symbol,
                    (int)argument_count, source_line);
}

static bool emit_new_object(BytecodeProgram *program, char *command, int source_line)
{
    RuntimeContext *runtime = program->runtime;
    const char *equals = strchr(command, '=');
    if (!equals)
        return false;
    const char *target = command;
    while (isspace((unsigned char)*target))
        target++;
    char variable_name[64];
    if (!read_call_identifier(&target, variable_name))
        return false;
    target = skip_call_space(target);
    if (*target++ != '=')
        return false;
    target = skip_call_space(target);
    if (strncmp(target, "new", 3) != 0 ||
        (isalnum((unsigned char)target[3]) || target[3] == '_'))
        return false;
    target = skip_call_space(target + 3);
    char class_name[64];
    if (!read_call_identifier(&target, class_name))
        return false;
    int class_index = find_class_index(runtime, class_name);
    if (class_index < 0)
    {
        report_diagnostic(runtime, "error", "E1305", "instantiated class is not defined",
                          source_line + 1, 1);
        return false;
    }
    char arguments[MAX_ACTION_PARAMETERS][MAX_LINE_LENGTH];
    size_t argument_count;
    if (!parse_call_arguments((char *)target, arguments, &argument_count, source_line,
                              runtime))
        return false;
    int constructor = find_class_method(runtime, class_index, "init");
    if ((constructor < 0 && argument_count != 0) ||
        (constructor >= 0 && runtime->funcs[constructor].parameter_count != argument_count))
    {
        report_diagnostic(runtime, "error", "E1306", "constructor argument count does not match init",
                          source_line + 1, 1);
        return false;
    }
    for (size_t i = 0; i < argument_count; i++)
    {
        if (!emit_expression_into(program, arguments[i], source_line))
            return false;
    }
    int target_symbol = intern_source(program, variable_name, source_line);
    if (target_symbol < 0 || program->instruction_count > (size_t)INT_MAX)
        return false;
    return emit_at3(program, BC_NEW, target_symbol, class_index,
                    (int)argument_count, source_line);
}

static bool looks_like_new_assignment(const char *command)
{
    const char *equals = strchr(command, '=');
    if (!equals)
        return false;
    const char *right = skip_call_space(equals + 1);
    return strncmp(right, "new", 3) == 0 &&
           !(isalnum((unsigned char)right[3]) || right[3] == '_');
}

static void strip_block_opener(char *header)
{
    bool quoted = false;
    bool escaped = false;
    for (char *cursor = header; *cursor; cursor++)
    {
        if (!quoted && *cursor == '#')
        {
            *cursor = '\0';
            break;
        }
        if (quoted && escaped)
        {
            escaped = false;
            continue;
        }
        if (quoted && *cursor == '\\')
        {
            escaped = true;
            continue;
        }
        if (*cursor == '"')
            quoted = !quoted;
        else if (!quoted && *cursor == '{')
        {
            *cursor = '\0';
            break;
        }
    }
    size_t length = strlen(header);
    while (length > 0 && (header[length - 1] == ' ' || header[length - 1] == '\t'))
        header[--length] = '\0';
}

static bool compile_range(BytecodeProgram *program, int first_line, int last_line,
                          unsigned int nesting, bool in_action)
{
    RuntimeContext *runtime = program->runtime;
    if (first_line > last_line)
        return true;
    if (nesting > MAX_STACK_DEPTH)
    {
        report_diagnostic(runtime, "error", "E1101", "source block nesting limit reached",
                          first_line + 1, 1);
        return false;
    }

    for (int line_index = first_line; line_index <= last_line; line_index++)
    {
        runtime->current_line = line_index + 1;
        char line[MAX_LINE_LENGTH];
        strncpy(line, runtime->source_lines[line_index], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        char *command = line;
        while (*command == ' ' || *command == '\t')
            command++;
        if (*command == '\0' || *command == '#' || *command == '{' || *command == '}')
            continue;

        ClassDefinition class_definition;
        if (parse_class_header(command, &class_definition))
        {
            if (in_action)
            {
                report_diagnostic(runtime, "error", "E1307", "class definitions are only allowed at the top level",
                                  line_index + 1, 1);
                return false;
            }
            int class_index = find_class_index(runtime, class_definition.name);
            int body_start, body_end, close_line;
            if (class_index < 0 ||
                !locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
                return false;
            for (size_t method_index = 0; method_index < runtime->func_count; method_index++)
            {
                Function *method = &runtime->funcs[method_index];
                if (strcmp(method->owner_class, class_definition.name) != 0)
                    continue;
                if (program->instruction_count > (size_t)INT_MAX)
                    return false;
                size_t skip_method = program->instruction_count;
                if (!emit_at(program, BC_JUMP, -1, 0, line_index))
                    return false;
                method->entry_pc = program->instruction_count;
                if (!compile_range(program, method->start_line, method->end_line - 1,
                                   nesting + 1, true) ||
                    !emit_at(program, BC_RETURN, 0, 0, method->end_line - 1))
                    return false;
                program->instructions[skip_method].operand = (int)program->instruction_count;
            }
            line_index = close_line;
            continue;
        }
        if (match_command(command, "class"))
        {
            report_diagnostic(runtime, "error", "E1308", "invalid class definition; expected class Name [extends Parent] {",
                              line_index + 1, 1);
            return false;
        }

        Function parsed_function;
        if (parse_action_header(command, &parsed_function))
        {
            if (in_action)
            {
                report_diagnostic(runtime, "error", "E1213", "nested action definitions are not supported",
                                  line_index + 1, 1);
                return false;
            }
            int function_index = find_function_index(runtime, parsed_function.name);
            int body_start;
            int body_end;
            int close_line;
            if (function_index < 0 ||
                !locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
            {
                report_diagnostic(runtime, "error", "E1201", "unable to locate action body",
                                  line_index + 1, 1);
                return false;
            }
            if (program->instruction_count > (size_t)INT_MAX)
                return false;
            size_t skip_jump = program->instruction_count;
            if (!emit_at(program, BC_JUMP, -1, 0, line_index))
                return false;
            runtime->funcs[function_index].entry_pc = program->instruction_count;
            if (!compile_range(program, body_start, body_end, nesting + 1, true) ||
                !emit_at(program, BC_RETURN, 0, 0, close_line))
                return false;
            if (program->instruction_count > (size_t)INT_MAX)
                return false;
            program->instructions[skip_jump].operand = (int)program->instruction_count;
            line_index = close_line;
            continue;
        }

        if (match_command(command, "action"))
        {
            report_diagnostic(runtime, "error", "E1215", "invalid action signature; expected action name(parameters) {",
                              line_index + 1, 1);
            return false;
        }

        if (match_command(command, "empty"))
        {
            const char *after_empty = command + 5;
            while (*after_empty == ' ' || *after_empty == '\t')
                after_empty++;
            if (*after_empty != '\0' && *after_empty != '#' && *after_empty != '\r' && *after_empty != '\n')
            {
                report_diagnostic(runtime, "error", "E1216", "empty is a standalone return statement, not an action modifier",
                                  line_index + 1, 1);
                return false;
            }
            if (!in_action)
            {
                report_diagnostic(runtime, "error", "E1214", "empty return is only valid inside an action",
                                  line_index + 1, 1);
                return false;
            }
            if (!emit_at(program, BC_RETURN, 0, 0, line_index))
                return false;
            continue;
        }

        if (match_command(command, "throw"))
        {
            char *expression = skip_cmd(command, "throw");
            if (*expression == '\0' || *expression == '#')
            {
                report_diagnostic(runtime, "error", "E1401", "throw requires an integer expression",
                                  line_index + 1, 1);
                return false;
            }
            if (!emit_expression_into(program, expression, line_index) ||
                !emit_at(program, BC_THROW, 0, 0, line_index))
                return false;
            continue;
        }

        if (match_command(command, "try"))
        {
            int try_start, try_end, try_close;
            if (!locate_block(runtime, line_index, last_line, &try_start, &try_end, &try_close))
            {
                report_diagnostic(runtime, "error", "E1402", "expected a balanced try block",
                                  line_index + 1, 1);
                return false;
            }

            int catch_line = try_close + 1;
            while (catch_line <= last_line)
            {
                const char *candidate = runtime->source_lines[catch_line];
                while (*candidate == ' ' || *candidate == '\t')
                    candidate++;
                if (*candidate == '\0' || *candidate == '#' ||
                    *candidate == '\r' || *candidate == '\n')
                {
                    catch_line++;
                    continue;
                }
                break;
            }
            char exception_name[64];
            int catch_start, catch_end, catch_close;
            if (catch_line > last_line ||
                !parse_catch_header(runtime->source_lines[catch_line], exception_name) ||
                !locate_block(runtime, catch_line, last_line, &catch_start, &catch_end, &catch_close))
            {
                report_diagnostic(runtime, "error", "E1403", "try must be followed by catch name { ... }",
                                  try_close + 1, 1);
                return false;
            }

            int exception_symbol = intern_source(program, exception_name, catch_line);
            if (exception_symbol < 0 || program->instruction_count > (size_t)INT_MAX)
                return false;
            size_t try_instruction = program->instruction_count;
            if (!emit_at3(program, BC_TRY, -1, exception_symbol, 0, line_index) ||
                !compile_range(program, try_start, try_end, nesting + 1, in_action) ||
                !emit_at(program, BC_END_TRY, 0, 0, try_close) ||
                program->instruction_count > (size_t)INT_MAX)
                return false;
            size_t skip_catch = program->instruction_count;
            if (!emit_at(program, BC_JUMP, -1, 0, try_close))
                return false;
            program->instructions[try_instruction].operand = (int)program->instruction_count;
            if (!compile_range(program, catch_start, catch_end, nesting + 1, in_action) ||
                program->instruction_count > (size_t)INT_MAX)
                return false;
            program->instructions[skip_catch].operand = (int)program->instruction_count;
            line_index = catch_close;
            continue;
        }
        if (match_command(command, "catch"))
        {
            report_diagnostic(runtime, "error", "E1404", "catch is only valid after a try block",
                              line_index + 1, 1);
            return false;
        }

        if (match_command(command, "call"))
        {
            report_diagnostic(runtime, "error", "E1217", "invoke actions directly as name(arguments); remove 'call'",
                              line_index + 1, 1);
            return false;
        }

        if (looks_like_method_invocation(command))
        {
            if (!emit_method_invocation(program, command, line_index))
                return false;
            continue;
        }

        if (looks_like_action_invocation(command))
        {
            if (!emit_action_invocation(program, command, line_index))
                return false;
            continue;
        }

        if (match_command(command, "while") || match_command(command, "when"))
        {
            bool is_while = match_command(command, "while");
            char *condition = skip_cmd(command, is_while ? "while" : "when");
            strip_block_opener(condition);
            int body_start;
            int body_end;
            int close_line;
            bool block_found = is_while
                                   ? locate_block(runtime, line_index, last_line, &body_start,
                                                  &body_end, &close_line)
                                   : locate_when_block(runtime, line_index, last_line, &body_start,
                                                      &body_end, &close_line);
            if (!block_found)
            {
                report_diagnostic(runtime, "error", "E1102", "expected a balanced control-flow block",
                                  line_index + 1, 1);
                return false;
            }

            int branch_index;
            if (is_while)
            {
                if (program->instruction_count > (size_t)INT_MAX)
                    return false;
                int condition_instruction = (int)program->instruction_count;
                if (!emit_expression_into(program, condition, line_index))
                    return false;
                if (program->instruction_count > (size_t)INT_MAX)
                    return false;
                branch_index = (int)program->instruction_count;
                if (!emit_at(program, BC_JUMP_IF_FALSE, -1, 0, line_index) ||
                    !compile_range(program, body_start, body_end, nesting + 1, in_action))
                    return false;
                if (program->instruction_count > (size_t)INT_MAX ||
                    !emit_at(program, BC_JUMP, condition_instruction, 0, line_index))
                    return false;
                program->instructions[branch_index].operand = (int)program->instruction_count;
                line_index = close_line;
                continue;
            }

            size_t inline_otherwise_brace = 0;
            bool inline_otherwise = find_inline_otherwise(
                runtime, line_index, close_line, &inline_otherwise_brace);
            int otherwise_line = close_line + 1;
            char otherwise_buffer[MAX_LINE_LENGTH] = "";
            bool has_otherwise = inline_otherwise;
            if (!inline_otherwise)
            {
                while (otherwise_line <= last_line)
                {
                    strncpy(otherwise_buffer, runtime->source_lines[otherwise_line], sizeof(otherwise_buffer) - 1);
                    otherwise_buffer[sizeof(otherwise_buffer) - 1] = '\0';
                    char *candidate = otherwise_buffer;
                    while (*candidate == ' ' || *candidate == '\t')
                        candidate++;
                    if (*candidate == '\0' || *candidate == '#' ||
                        *candidate == '\r' || *candidate == '\n')
                    {
                        otherwise_line++;
                        continue;
                    }
                    break;
                }
                has_otherwise = otherwise_line <= last_line &&
                                match_command(otherwise_buffer, "otherwise");
            }
            int otherwise_start = 0;
            int otherwise_end = -1;
            int otherwise_close = close_line;
            if (has_otherwise &&
                !(inline_otherwise
                      ? locate_inline_otherwise_block(runtime, close_line, inline_otherwise_brace,
                                                      last_line, &otherwise_start,
                                                      &otherwise_end, &otherwise_close)
                      : locate_block(runtime, otherwise_line, last_line, &otherwise_start,
                                     &otherwise_end, &otherwise_close)))
            {
                report_diagnostic(runtime, "error", "E1102", "expected a balanced otherwise block",
                                  (inline_otherwise ? close_line : otherwise_line) + 1, 1);
                return false;
            }

            if (!emit_expression_into(program, condition, line_index))
                return false;
            if (program->instruction_count > (size_t)INT_MAX)
                return false;
            branch_index = (int)program->instruction_count;
            if (!emit_at(program, BC_JUMP_IF_FALSE, -1, 0, line_index) ||
                !compile_range(program, body_start, body_end, nesting + 1, in_action))
                return false;

            if (has_otherwise)
            {
                if (program->instruction_count > (size_t)INT_MAX)
                    return false;
                int end_jump = (int)program->instruction_count;
                if (!emit_at(program, BC_JUMP, -1, 0, line_index))
                    return false;
                if (!patch_jump(program, branch_index, program->instruction_count))
                    return false;
                if (!compile_range(program, otherwise_start, otherwise_end, nesting + 1, in_action))
                    return false;
                if (!patch_jump(program, end_jump, program->instruction_count))
                    return false;
                line_index = otherwise_close;
            }
            else
            {
                if (!patch_jump(program, branch_index, program->instruction_count))
                    return false;
                line_index = close_line;
            }
            continue;
        }

        if (match_command(command, "repeat"))
        {
            char *count_expression = skip_cmd(command, "repeat");
            strip_block_opener(count_expression);
            int body_start;
            int body_end;
            int close_line;
            if (!locate_block(runtime, line_index, last_line, &body_start, &body_end, &close_line))
            {
                report_diagnostic(runtime, "error", "E1102", "expected a balanced repeat block",
                                  line_index + 1, 1);
                return false;
            }
            if (!emit_expression_into(program, count_expression, line_index))
                return false;
            if (program->instruction_count > (size_t)INT_MAX)
                return false;
            int repeat_init = (int)program->instruction_count;
            if (!emit_at(program, BC_REPEAT_INIT, -1, 0, line_index))
                return false;
            int body_instruction = (int)program->instruction_count;
            if (!compile_range(program, body_start, body_end, nesting + 1, in_action) ||
                !emit_at(program, BC_REPEAT_NEXT, body_instruction, 0, line_index))
                return false;
            if (program->instruction_count > (size_t)INT_MAX)
                return false;
            program->instructions[repeat_init].operand = (int)program->instruction_count;
            line_index = close_line;
            continue;
        }

        if (match_command(command, "input"))
        {
            if (!emit_input_statement(program, command, line_index))
                return false;
            continue;
        }

        if (match_command(command, "matrix") || match_command(command, "matrix_set") ||
            match_command(command, "matrix_get") || match_command(command, "queue") ||
            match_command(command, "enqueue") || match_command(command, "dequeue") ||
            match_command(command, "peek") || match_command(command, "len"))
        {
            bool handled;
            if (!emit_collection_command(program, command, line_index, &handled))
                return false;
            if (handled)
                continue;
        }

        bool managed_statement;
        if (!emit_managed_statement(program, command, line_index, &managed_statement))
            return false;
        if (managed_statement)
            continue;

        if (match_command(command, "display"))
        {
            if (!emit_display_statement(program, skip_cmd(command, "display"), line_index))
                return false;
            continue;
        }

        bool typed_declaration;
        if (!emit_typed_declaration(program, command, line_index,
                                    &typed_declaration))
            return false;
        if (typed_declaration)
            continue;

        bool builtin = false;
        if (!builtin && looks_like_new_assignment(command))
        {
            if (!emit_new_object(program, command, line_index))
            {
                report_diagnostic(runtime, "error", "E1309", "invalid object creation; expected name = new Class(arguments)",
                                  line_index + 1, 1);
                return false;
            }
            continue;
        }
        if (!builtin && valid_assignment_syntax(command))
        {
            bool handled;
            if (!emit_value_assignment(program, command, line_index, &handled))
            {
                if (!handled)
                    report_diagnostic(runtime, "error", "E1001", "invalid value assignment",
                                      line_index + 1, 1);
                return false;
            }
            if (handled)
                continue;
        }
        report_diagnostic(runtime, "error", "E1218",
                          "unrecognized statement; expected a built-in, assignment, or action invocation",
                          line_index + 1, 1);
        return false;
    }
    return true;
}

bool bytecode_compile_program(RuntimeContext *runtime, int first_line,
                              int last_line, BytecodeProgram *program)
{
    if (!runtime_is_initialized(runtime) || !program || first_line < 0 ||
        last_line < first_line ||
        (size_t)last_line >= runtime->source_line_count)
        return false;
    memset(program, 0, sizeof(*program));
    program->runtime = runtime;
    if (!register_classes(runtime, first_line, last_line) ||
        !register_actions(runtime, first_line, last_line) ||
        !compile_range(program, first_line, last_line, 0, false))
    {
        bytecode_free(program);
        return false;
    }
    if (!emit_at(program, BC_HALT, 0, 0, last_line))
    {
        bytecode_free(program);
        return false;
    }
    program->has_result = false;
    return true;
}

static bool execute_input(RuntimeContext *runtime, const BytecodeProgram *program,
                          const BytecodeInstruction *instruction)
{
    if (instruction->operand < 0 ||
        (size_t)instruction->operand >= program->symbol_count ||
        instruction->operand2 <= 0 ||
        instruction->operand2 > (MAX_LINE_LENGTH + 1) / 2)
        return false;

    char *names[(MAX_LINE_LENGTH + 1) / 2];
    size_t name_count = 0;
    char encoded_names[MAX_LINE_LENGTH];
    size_t encoded_length = strlen(program->symbols[instruction->operand]);
    if (encoded_length >= sizeof(encoded_names))
        return false;
    memcpy(encoded_names, program->symbols[instruction->operand], encoded_length + 1);
    if (!parse_input_names(encoded_names, names,
                           sizeof(names) / sizeof(names[0]), &name_count) ||
        name_count != (size_t)instruction->operand2)
        return false;

    char input_line[MAX_LINE_LENGTH];
    fputs("? ", stdout);
    fflush(stdout);
    if (!fgets(input_line, sizeof(input_line), stdin))
    {
        runtime_error(runtime, instruction->column, "Unable to read an input line.");
        return false;
    }
    if (!strchr(input_line, '\n') && !feof(stdin))
    {
        int character;
        while ((character = getchar()) != '\n' && character != EOF)
            ;
        runtime_error(runtime, instruction->column, "Input line exceeds the 1023-character limit.");
        return false;
    }

    input_line[strcspn(input_line, "\r\n")] = '\0';
    char *tokens[(MAX_LINE_LENGTH + 1) / 2];
    size_t token_count = 0;
    if (!parse_input_values(input_line, tokens,
                            sizeof(tokens) / sizeof(tokens[0]), &token_count))
    {
        runtime_error(runtime, instruction->column,
                      "Input values must be tokens or quoted strings shorter than 256 bytes.");
        return false;
    }
    if (token_count != name_count)
    {
        char message[128];
        snprintf(message, sizeof(message), "Input expected %lu value(s), received %lu.",
                 (unsigned long)name_count, (unsigned long)token_count);
        runtime_error(runtime, instruction->column, message);
        return false;
    }

    Value *values = calloc(name_count, sizeof(*values));
    if (!values)
    {
        runtime_error(runtime, instruction->column, "Unable to allocate parsed input values.");
        return false;
    }
    VMStackRoot values_root = {values, 0};
    if (!gc_root_add(runtime, &values_root, trace_vm_stack))
    {
        free(values);
        runtime_error(runtime, instruction->column, "Unable to root parsed input values.");
        return false;
    }

    bool success = true;
    for (size_t i = 0; i < name_count; i++)
    {
        errno = 0;
        char *end = NULL;
        long number = strtol(tokens[i], &end, 10);
        bool is_integer = tokens[i][0] != '\0' && end && *end == '\0' &&
                          errno != ERANGE && number >= INT_MIN && number <= INT_MAX;
        if (is_integer)
            values[i] = value_int(number);
        else if (!value_string(runtime, tokens[i], strlen(tokens[i]), &values[i]))
        {
            runtime_error(runtime, instruction->column, "Unable to allocate input string value.");
            success = false;
            break;
        }
        values_root.depth++;
    }

    if (success)
    {
        for (size_t i = 0; i < name_count; i++)
        {
            Variable *variable = get_or_create_var(runtime, names[i]);
            if (!variable_replace_runtime_value(variable, &values[i]))
            {
                runtime_error(runtime, instruction->column, "Unable to store input value.");
                success = false;
                break;
            }
        }
    }
    if (success)
        save_state_to_persistence(runtime);

    if (!gc_root_remove(runtime, &values_root))
    {
        runtime_error(runtime, instruction->column, "Unable to remove temporary input roots.");
        success = false;
    }
    for (size_t i = 0; i < values_root.depth; i++)
        value_release(&values[i]);
    free(values);
    return success;
}

static bool execute_stdlib_call(RuntimeContext *runtime,
                                const BytecodeInstruction *instruction,
                                Value *arguments, size_t argument_count,
                                Value *result,
                                char *error, size_t error_capacity)
{
    if (!instruction || !arguments || !result)
        return false;
    switch ((BytecodeStdlibFunction)instruction->operand)
    {
    case BC_STDLIB_ABS:
        if (argument_count != 1)
            break;
        if (arguments[0].type == VALUE_INT)
        {
            if (arguments[0].as.integer == INT64_MIN)
                snprintf(error, error_capacity, "abs cannot represent INT64_MIN.");
            else
                *result = value_int(arguments[0].as.integer < 0
                                        ? -arguments[0].as.integer
                                        : arguments[0].as.integer);
            return arguments[0].as.integer != INT64_MIN;
        }
        if (arguments[0].type == VALUE_FLOAT)
        {
            double number = arguments[0].as.floating;
            *result = value_float(number < 0.0 ? -number : number);
            return true;
        }
        snprintf(error, error_capacity, "abs expects an integer or float.");
        return false;
    case BC_STDLIB_MIN:
    case BC_STDLIB_MAX:
    {
        if (argument_count != 2)
            break;
        Value comparison = value_null();
        if (!value_binary(runtime, VALUE_OP_LESS, &arguments[0], &arguments[1],
                          &comparison, error, error_capacity))
            return false;
        bool choose_left = comparison.as.boolean ==
                           (instruction->operand == BC_STDLIB_MIN);
        return value_copy(result, &arguments[choose_left ? 0 : 1]);
    }
    case BC_STDLIB_LEN:
    {
        if (argument_count != 1)
            break;
        size_t length;
        switch (arguments[0].type)
        {
        case VALUE_ARRAY:
            length = value_array_length(&arguments[0]);
            break;
        case VALUE_MATRIX:
            length = value_matrix_rows(&arguments[0]) *
                     value_matrix_columns(&arguments[0]);
            break;
        case VALUE_QUEUE:
            length = value_queue_length(&arguments[0]);
            break;
        case VALUE_MAP:
            length = value_map_length(&arguments[0]);
            break;
        case VALUE_SET:
            length = value_set_length(&arguments[0]);
            break;
        case VALUE_STRING:
        {
            size_t byte_length = 0;
            if (!value_string_bytes(&arguments[0], &byte_length))
            {
                snprintf(error, error_capacity, "Invalid string value.");
                return false;
            }
            length = byte_length;
            break;
        }
        default:
            snprintf(error, error_capacity,
                     "len expects a string, array, matrix, queue, map, or set.");
            return false;
        }
        if (length > (size_t)INT64_MAX)
        {
            snprintf(error, error_capacity, "Collection length exceeds integer range.");
            return false;
        }
        *result = value_int((int64_t)length);
        return true;
    }
    case BC_STDLIB_LOWER:
    case BC_STDLIB_UPPER:
    {
        if (argument_count != 1 || arguments[0].type != VALUE_STRING)
        {
            snprintf(error, error_capacity, "%s expects a string.",
                     instruction->operand == BC_STDLIB_LOWER ? "lower" : "upper");
            return false;
        }
        size_t length = 0;
        const char *bytes = value_string_bytes(&arguments[0], &length);
        if (!bytes || length == SIZE_MAX)
        {
            snprintf(error, error_capacity, "Invalid string value.");
            return false;
        }
        char *converted = malloc(length + 1);
        if (!converted)
        {
            snprintf(error, error_capacity, "Unable to allocate transformed string.");
            return false;
        }
        for (size_t i = 0; i < length; i++)
        {
            unsigned char character = (unsigned char)bytes[i];
            if (instruction->operand == BC_STDLIB_LOWER &&
                character >= 'A' && character <= 'Z')
                character = (unsigned char)(character + ('a' - 'A'));
            else if (instruction->operand == BC_STDLIB_UPPER &&
                     character >= 'a' && character <= 'z')
                character = (unsigned char)(character - ('a' - 'A'));
            converted[i] = (char)character;
        }
        converted[length] = '\0';
        bool success = value_string(runtime, converted, length, result);
        free(converted);
        if (!success)
            snprintf(error, error_capacity, "Unable to allocate transformed string.");
        return success;
    }
    case BC_STDLIB_SUBSTR:
    {
        if (argument_count != 3 || arguments[0].type != VALUE_STRING ||
            arguments[1].type != VALUE_INT || arguments[2].type != VALUE_INT ||
            arguments[1].as.integer < 0 || arguments[2].as.integer < 0)
        {
            snprintf(error, error_capacity,
                     "substr expects a string, a nonnegative start, and a nonnegative length.");
            return false;
        }
        size_t length = 0;
        const char *bytes = value_string_bytes(&arguments[0], &length);
        uint64_t start = (uint64_t)arguments[1].as.integer;
        uint64_t count = (uint64_t)arguments[2].as.integer;
        if (!bytes || start > length || count > length - (size_t)start)
        {
            snprintf(error, error_capacity, "substr range is outside the string.");
            return false;
        }
        if (!value_string(runtime, bytes + (size_t)start, (size_t)count, result))
        {
            snprintf(error, error_capacity, "Unable to allocate substring.");
            return false;
        }
        return true;
    }
    case BC_STDLIB_TRIM:
        if (argument_count != 1)
            break;
        return stdlib_string_trim(runtime, &arguments[0], result, error,
                                  error_capacity);
    case BC_STDLIB_STARTS_WITH:
    case BC_STDLIB_ENDS_WITH:
    {
        bool matches = false;
        if (argument_count != 2)
            break;
        bool success = instruction->operand == BC_STDLIB_STARTS_WITH
                           ? stdlib_string_starts_with(
                                 &arguments[0], &arguments[1], &matches, error,
                                 error_capacity)
                           : stdlib_string_ends_with(
                                 &arguments[0], &arguments[1], &matches, error,
                                 error_capacity);
        if (success)
            *result = value_bool(matches);
        return success;
    }
    case BC_STDLIB_CONTAINS:
    {
        bool matches = false;
        if (argument_count != 2)
            break;
        if (!stdlib_string_contains(&arguments[0], &arguments[1], &matches,
                                    error, error_capacity))
            return false;
        *result = value_bool(matches);
        return true;
    }
    case BC_STDLIB_INDEX_OF:
        if (argument_count != 2)
            break;
        return stdlib_string_index_of(&arguments[0], &arguments[1], result,
                                      error, error_capacity);
    case BC_STDLIB_COUNT:
        if (argument_count != 2)
            break;
        return stdlib_string_count(&arguments[0], &arguments[1], result, error,
                                   error_capacity);
    case BC_STDLIB_REPLACE:
        if (argument_count != 3)
            break;
        return stdlib_string_replace(runtime, &arguments[0], &arguments[1],
                                     &arguments[2], result, error,
                                     error_capacity);
    case BC_STDLIB_REPEAT:
        if (argument_count != 2)
            break;
        return stdlib_string_repeat(runtime, &arguments[0], &arguments[1],
                                    result, error, error_capacity);
    case BC_STDLIB_SQRT:
    case BC_STDLIB_SIN:
    case BC_STDLIB_COS:
    case BC_STDLIB_LOG:
    case BC_STDLIB_EXP:
    case BC_STDLIB_FLOOR:
    case BC_STDLIB_CEIL:
    case BC_STDLIB_ROUND:
    {
        static const StdMathFunction math_functions[] = {
            STD_MATH_SQRT, STD_MATH_SIN, STD_MATH_COS, STD_MATH_LOG,
            STD_MATH_EXP, STD_MATH_FLOOR, STD_MATH_CEIL, STD_MATH_ROUND};
        int index = instruction->operand - BC_STDLIB_SQRT;
        if (argument_count != 1 || index < 0 ||
            (size_t)index >= sizeof(math_functions) / sizeof(math_functions[0]))
            break;
        return stdlib_math_unary(math_functions[index], &arguments[0], result,
                                 error, error_capacity);
    }
    case BC_STDLIB_PI:
        if (argument_count != 0)
            break;
        *result = value_float(3.14159265358979323846);
        return true;
    case BC_STDLIB_RANDOM_INT:
        if (argument_count != 2)
            break;
        return stdlib_random_int(runtime, &arguments[0], &arguments[1], result,
                                 error, error_capacity);
    case BC_STDLIB_RANDOM_SEED:
        if (argument_count != 1)
            break;
        return stdlib_random_seed(runtime, &arguments[0], result, error,
                                  error_capacity);
    }
    snprintf(error, error_capacity, "Invalid standard-library call bytecode.");
    return false;
}

#define MAX_VM_COLLECTION_ELEMENTS 1000000U

static bool bytecode_execute_internal(RuntimeContext *runtime,
                                      const BytecodeProgram *program,
                                      int *integer_result, Value *value_result)
{
    if (!runtime_is_initialized(runtime) || !program ||
        program->runtime != runtime ||
        (program->has_result && !integer_result && !value_result) ||
        program->instruction_count == 0 ||
        program->instruction_count >= SIZE_MAX / sizeof(Value))
        return false;

    size_t stack_capacity = program->instruction_count + 1;
    Value *stack = calloc(stack_capacity, sizeof(*stack));
    if (!stack)
    {
        runtime_error(runtime, 1, "Unable to allocate expression stack.");
        return false;
    }
    VMStackRoot stack_root = {stack, 0};
    if (!gc_root_add(runtime, &stack_root, trace_vm_stack))
    {
        free(stack);
        runtime_error(runtime, 1, "Unable to register VM operand stack with garbage collector.");
        return false;
    }

    size_t depth = 0;
    bool success = false;
    int initial_stack_top = runtime->stack_top;
    CallFrame call_frames[MAX_STACK_DEPTH];
    size_t call_depth = 0;
    RepeatFrame *repeat_frames = NULL;
    size_t repeat_depth = 0;
    size_t repeat_capacity = 0;
    ExceptionFrame exception_frames[MAX_STACK_DEPTH];
    size_t exception_depth = 0;
    size_t pc = 0;
    while (pc < program->instruction_count)
    {
        stack_root.depth = depth;
        if (!gc_safe_point(runtime))
        {
            runtime_error(runtime, 1, "Garbage collection failed at a VM safe point.");
            goto done;
        }
        if (!runtime_consume_execution_step(runtime))
            goto done;
        runtime_record_instruction(runtime);
        const BytecodeInstruction *instruction = &program->instructions[pc];
        if (instruction->line > 0)
            runtime->current_line = instruction->line;
        switch (instruction->opcode)
        {
        case BC_PUSH_INT:
            if (depth >= stack_capacity)
                goto invalid_bytecode;
            stack[depth] = value_int(instruction->operand);
            depth++;
            break;
        case BC_PUSH_BOOL:
            if (depth >= stack_capacity)
                goto invalid_bytecode;
            stack[depth++] = value_bool(instruction->operand != 0);
            break;
        case BC_PUSH_NULL:
            if (depth >= stack_capacity)
                goto invalid_bytecode;
            stack[depth++] = value_null();
            break;
        case BC_PUSH_STRING:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                depth >= stack_capacity)
                goto invalid_bytecode;
            if (!value_string(runtime, program->symbols[instruction->operand],
                              strlen(program->symbols[instruction->operand]),
                              &stack[depth]))
            {
                runtime_error(runtime, instruction->column, "Unable to allocate string operand.");
                goto done;
            }
            depth++;
            break;
        case BC_LOAD_VAR:
        {
            if (instruction->operand < 0 || (size_t)instruction->operand >= program->symbol_count ||
                depth >= stack_capacity)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!load_variable_value(runtime, variable, &stack[depth]))
            {
                runtime_error(runtime, instruction->column, "Unable to load variable value.");
                goto done;
            }
            depth++;
            break;
        }
        case BC_INPUT:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                instruction->operand2 <= 0 ||
                instruction->operand2 > (MAX_LINE_LENGTH + 1) / 2)
                goto invalid_bytecode;
            if (!execute_input(runtime, program, instruction))
                goto done;
            break;
        case BC_LIST_CREATE:
        case BC_PILE_CREATE:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count)
                goto invalid_bytecode;
            Value array = value_null();
            if (!value_array(runtime, &array) || !value_root_add(runtime, &array))
            {
                runtime_error(runtime, instruction->column,
                              "Unable to allocate or root managed list.");
                goto done;
            }
            Variable *variable = get_or_create_var(runtime, program->symbols[instruction->operand]);
            bool stored = variable_replace_runtime_value(variable, &array);
            if (stored)
                variable->collection_kind =
                    instruction->opcode == BC_PILE_CREATE
                        ? VARIABLE_COLLECTION_PILE
                        : VARIABLE_COLLECTION_LIST;
            bool unrooted = value_root_remove(runtime, &array);
            value_release(&array);
            if (!stored || !unrooted)
            {
                runtime_error(runtime, instruction->column, "Unable to initialize managed list.");
                goto done;
            }
            save_state_to_persistence(runtime);
            break;
        }
        case BC_COLLECTION_PUSH:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth == 0)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!variable || !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_ARRAY)
            {
                runtime_error(runtime, instruction->column,
                              "push requires an initialized list or pile.");
                goto done;
            }
            if (value_array_length(&variable->runtime_value) >=
                    MAX_VM_COLLECTION_ELEMENTS ||
                !value_array_push(&variable->runtime_value, &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column,
                              "Unable to append to list or pile; collection limit or allocation failure.");
                goto done;
            }
            depth--;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_COLLECTION_POP:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!variable || !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_ARRAY)
            {
                runtime_error(runtime, instruction->column,
                              "pop requires an initialized list or pile.");
                goto done;
            }
            Value discarded = value_null();
            if (value_array_pop(&variable->runtime_value, &discarded))
            {
                value_release(&discarded);
                save_state_to_persistence(runtime);
            }
            break;
        }
        case BC_COLLECTION_SAVE:
        case BC_COLLECTION_LOAD:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count)
                goto invalid_bytecode;
            bool success = instruction->opcode == BC_COLLECTION_SAVE
                               ? runtime_collection_save(
                                     runtime, program->symbols[instruction->operand],
                                     program->symbols[instruction->operand2])
                               : runtime_collection_load(
                                     runtime, program->symbols[instruction->operand],
                                     program->symbols[instruction->operand2]);
            if (!success)
                goto done;
            break;
        }
        case BC_FILE_DISPLAY:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count)
                goto invalid_bytecode;
            if (!runtime_file_display(runtime, program->symbols[instruction->operand]))
                goto done;
            break;
        case BC_FILE_READ:
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count)
                goto invalid_bytecode;
            if (!runtime_file_read(runtime, program->symbols[instruction->operand],
                                   program->symbols[instruction->operand2]))
                goto done;
            break;
        case BC_FILE_WRITE:
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count ||
                (instruction->operand3 != 0 && instruction->operand3 != 1))
                goto invalid_bytecode;
            if (!runtime_file_write(runtime, program->symbols[instruction->operand],
                                    program->symbols[instruction->operand2],
                                    instruction->operand3 != 0))
                goto done;
            break;
        case BC_MATRIX_CREATE:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth < 2)
                goto invalid_bytecode;
            size_t rows, columns;
            if (!operand_size(&stack[depth - 2], &rows) ||
                !operand_size(&stack[depth - 1], &columns) ||
                rows == 0 || columns == 0 ||
                rows > MAX_VM_COLLECTION_ELEMENTS / columns ||
                rows * columns > (size_t)INT_MAX)
            {
                runtime_error(runtime, instruction->column,
                              "Matrix dimensions must be positive and within the element limit.");
                goto done;
            }
            Value matrix = value_null();
            if (!value_matrix(runtime, rows, columns, &matrix))
            {
                runtime_error(runtime, instruction->column, "Unable to allocate managed matrix.");
                goto done;
            }
            Variable *variable = get_or_create_var(runtime, program->symbols[instruction->operand]);
            if (!variable_replace_runtime_value(variable, &matrix))
            {
                runtime_error(runtime, instruction->column, "Unable to store managed matrix.");
                goto done;
            }
            depth -= 2;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_MATRIX_SET:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth < 3)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            size_t row, column;
            if (!operand_size(&stack[depth - 3], &row) ||
                !operand_size(&stack[depth - 2], &column) ||
                stack[depth - 1].type != VALUE_INT || !variable ||
                !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_MATRIX ||
                !value_matrix_set(&variable->runtime_value, row, column,
                                  &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column,
                              "matrix_set requires an initialized matrix, nonnegative indices, and an integer value in bounds.");
                goto done;
            }
            depth -= 3;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_MATRIX_GET:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count || depth < 2)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            size_t row, column;
            Value result = value_null();
            if (!operand_size(&stack[depth - 2], &row) ||
                !operand_size(&stack[depth - 1], &column) || !variable ||
                !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_MATRIX ||
                !value_matrix_get(&variable->runtime_value, row, column, &result))
            {
                runtime_error(runtime, instruction->column,
                              "matrix_get requires an initialized matrix and in-bounds nonnegative indices.");
                goto done;
            }
            Variable *target = get_or_create_var(runtime, program->symbols[instruction->operand2]);
            if (!variable_replace_runtime_value(target, &result))
            {
                runtime_error(runtime, instruction->column, "Unable to store matrix value.");
                goto done;
            }
            depth -= 2;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_QUEUE_CREATE:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count)
                goto invalid_bytecode;
            Value queue = value_null();
            if (!value_queue(runtime, &queue))
            {
                runtime_error(runtime, instruction->column, "Unable to allocate managed queue.");
                goto done;
            }
            Variable *variable = get_or_create_var(runtime, program->symbols[instruction->operand]);
            if (!variable_replace_runtime_value(variable, &queue))
            {
                runtime_error(runtime, instruction->column, "Unable to store managed queue.");
                goto done;
            }
            save_state_to_persistence(runtime);
            break;
        }
        case BC_QUEUE_ENQUEUE:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth == 0)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!variable || !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_QUEUE ||
                stack[depth - 1].type != VALUE_INT ||
                value_queue_length(&variable->runtime_value) >=
                    MAX_VM_COLLECTION_ELEMENTS ||
                !value_queue_enqueue(&variable->runtime_value, &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column,
                              "enqueue requires an initialized queue and an integer value.");
                goto done;
            }
            depth--;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_QUEUE_DEQUEUE:
        case BC_QUEUE_PEEK:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            Value result = value_null();
            bool success = variable && variable->has_runtime_value &&
                           variable->runtime_value.type == VALUE_QUEUE &&
                           (instruction->opcode == BC_QUEUE_DEQUEUE
                                ? value_queue_dequeue(&variable->runtime_value, &result)
                                : value_queue_peek(&variable->runtime_value, &result));
            if (!success)
            {
                runtime_error(runtime, instruction->column,
                              "dequeue/peek requires a nonempty initialized queue.");
                goto done;
            }
            Variable *target = get_or_create_var(runtime, program->symbols[instruction->operand2]);
            if (!variable_replace_runtime_value(target, &result))
            {
                runtime_error(runtime, instruction->column, "Unable to store queue value.");
                goto done;
            }
            if (instruction->opcode == BC_QUEUE_DEQUEUE)
                save_state_to_persistence(runtime);
            break;
        }
        case BC_COLLECTION_LEN:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count)
                goto invalid_bytecode;
            Variable *source = get_var_obj(runtime, program->symbols[instruction->operand]);
            size_t length;
            if (source && source->has_runtime_value &&
                source->runtime_value.type == VALUE_MATRIX)
                length = value_matrix_rows(&source->runtime_value) *
                         value_matrix_columns(&source->runtime_value);
            else if (source && source->has_runtime_value &&
                     source->runtime_value.type == VALUE_QUEUE)
                length = value_queue_length(&source->runtime_value);
            else if (source && source->has_runtime_value &&
                     source->runtime_value.type == VALUE_ARRAY)
                length = value_array_length(&source->runtime_value);
            else if (source && source->has_runtime_value &&
                     source->runtime_value.type == VALUE_MAP)
                length = value_map_length(&source->runtime_value);
            else if (source && source->has_runtime_value &&
                     source->runtime_value.type == VALUE_SET)
                length = value_set_length(&source->runtime_value);
            else
            {
                runtime_error(runtime, instruction->column,
                              "len requires an initialized collection.");
                goto done;
            }
            Variable *target = get_or_create_var(runtime, program->symbols[instruction->operand2]);
            Value result = value_int((int64_t)length);
            if (!variable_replace_runtime_value(target, &result))
            {
                runtime_error(runtime, instruction->column, "Unable to store collection length.");
                goto done;
            }
            save_state_to_persistence(runtime);
            break;
        }
        case BC_DISPLAY_VALUE:
        {
            if (depth == 0 ||
                (instruction->operand >= 0 &&
                 (size_t)instruction->operand >= program->symbol_count))
                goto invalid_bytecode;
            if (instruction->operand >= 0 &&
                runtime_display_special(runtime, program->symbols[instruction->operand]))
                depth--;
            else
            {
                print_value(&stack[depth - 1]);
                depth--;
            }
            break;
        }
        case BC_DISPLAY_SPECIAL:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count)
                goto invalid_bytecode;
            if (!runtime_display_special(runtime, program->symbols[instruction->operand]))
            {
                runtime_error(runtime, instruction->column, "Unable to display legacy object property.");
                goto done;
            }
            break;
        case BC_DISPLAY_NEWLINE:
            if (depth != 0)
                goto invalid_bytecode;
            putchar('\n');
            break;
        case BC_LOAD_FIELD:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                depth >= stack_capacity)
                goto invalid_bytecode;
            const char *path = program->symbols[instruction->operand];
            const char *dot = strchr(path, '.');
            if (!dot || dot == path || !dot[1] || strchr(dot + 1, '.') ||
                (size_t)(dot - path) >= 64 || strlen(dot + 1) >= 64)
                goto invalid_bytecode;
            char receiver_name[64];
            size_t receiver_length = (size_t)(dot - path);
            memcpy(receiver_name, path, receiver_length);
            receiver_name[receiver_length] = '\0';
            Variable *receiver = get_var_obj(runtime, receiver_name);
            if (!receiver || !receiver->has_runtime_value ||
                receiver->runtime_value.type != VALUE_OBJECT ||
                !runtime_object_field_get(runtime, &receiver->runtime_value, dot + 1,
                                          &stack[depth]))
            {
                runtime_error(runtime, instruction->column, "Unknown object property.");
                goto done;
            }
            depth++;
            break;
        }
        case BC_STORE_FIELD:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count || depth == 0)
                goto invalid_bytecode;
            Variable *receiver = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!receiver || !receiver->has_runtime_value ||
                receiver->runtime_value.type != VALUE_OBJECT ||
                !runtime_object_field_set(runtime, &receiver->runtime_value,
                                         program->symbols[instruction->operand2],
                                         &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column, "Unknown object property or invalid field value.");
                goto done;
            }
            depth--;
            save_state_to_persistence(runtime);
            break;
        }
        case BC_LIST_GET:
        {
            if (instruction->operand < 0 || (size_t)instruction->operand >= program->symbol_count || depth == 0)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (variable && variable->has_runtime_value &&
                variable->runtime_value.type == VALUE_MAP)
            {
                Value loaded = value_null();
                if (!value_map_get(&variable->runtime_value, &stack[depth - 1], &loaded))
                {
                    runtime_error(runtime, instruction->column, "Map key is invalid or does not exist.");
                    goto done;
                }
                stack[depth - 1] = loaded;
            }
            else if (variable && variable->has_runtime_value &&
                     variable->runtime_value.type == VALUE_ARRAY)
            {
                int index = operand_integer(&stack[depth - 1]);
                Value loaded = value_null();
                if (index < 0 || !value_array_get(&variable->runtime_value,
                                                  (size_t)index, &loaded))
                {
                    if (variable->collection_kind == VARIABLE_COLLECTION_LIST ||
                        variable->collection_kind == VARIABLE_COLLECTION_PILE)
                        stack[depth - 1] = value_int(0);
                    else
                    {
                        runtime_error(runtime, instruction->column,
                                      "Array index is invalid or out of range.");
                        goto done;
                    }
                }
                else
                    stack[depth - 1] = loaded;
            }
            else
            {
                stack[depth - 1] = value_int(0);
            }
            break;
        }
        case BC_BUILD_MAP:
        case BC_BUILD_SET:
        {
            if (instruction->operand < 0)
                goto invalid_bytecode;
            bool is_map = instruction->opcode == BC_BUILD_MAP;
            size_t item_count = (size_t)instruction->operand;
            if (is_map && item_count > SIZE_MAX / 2)
                goto invalid_bytecode;
            size_t values_needed = item_count * (is_map ? 2U : 1U);
            if (values_needed > depth || depth >= stack_capacity)
                goto invalid_bytecode;
            Value collection = value_null();
            if (!(is_map ? value_map(runtime, &collection) : value_set(runtime, &collection)))
            {
                runtime_error(runtime, instruction->column, "Unable to allocate collection literal.");
                goto done;
            }
            size_t start = depth - values_needed;
            for (size_t i = 0; i < item_count; i++)
            {
                bool inserted = false;
                bool success = is_map
                                   ? value_map_set(&collection, &stack[start + i * 2],
                                                   &stack[start + i * 2 + 1])
                                   : value_set_add(&collection, &stack[start + i], &inserted);
                if (!success)
                {
                    runtime_error(runtime, instruction->column,
                                  is_map ? "Map literal contains an invalid key."
                                         : "Set literal contains an invalid value.");
                    goto done;
                }
            }
            depth = start;
            stack[depth++] = collection;
            break;
        }
        case BC_MAP_SET:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth < 2)
                goto invalid_bytecode;
            Variable *variable = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!variable || !variable->has_runtime_value ||
                variable->runtime_value.type != VALUE_MAP ||
                !value_map_set(&variable->runtime_value, &stack[depth - 2],
                               &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column, "Indexed assignment requires a map and a valid key.");
                goto done;
            }
            depth -= 2;
            break;
        }
        case BC_STORE_VAR:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count || depth == 0)
                goto invalid_bytecode;
            Variable *variable = get_or_create_var(runtime, program->symbols[instruction->operand]);
            if (!variable_replace_runtime_value(variable, &stack[depth - 1]))
            {
                runtime_error(runtime, instruction->column, "Unable to assign runtime value.");
                goto done;
            }
            save_state_to_persistence(runtime);
            depth--;
            break;
        }
        case BC_DECLARE_VAR:
        {
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                instruction->operand2 < VARIABLE_TYPE_INT ||
                instruction->operand2 > VARIABLE_TYPE_NULL || depth == 0)
                goto invalid_bytecode;
            Variable *variable =
                get_or_create_var(runtime, program->symbols[instruction->operand]);
            VariableDeclaredType declared_type =
                (VariableDeclaredType)instruction->operand2;
            if (!variable_declare_and_set_runtime_value(
                    variable, declared_type, &stack[depth - 1]))
            {
                char message[192];
                if (variable &&
                    variable->declared_type != VARIABLE_TYPE_UNDECLARED)
                    (void)snprintf(message, sizeof(message),
                                   "variable '%s' already has a declared type",
                                   program->symbols[instruction->operand]);
                else
                    (void)snprintf(
                        message, sizeof(message),
                        "initializer has runtime type %s; declaration requires %s",
                        value_type_name(stack[depth - 1].type),
                        typecheck_type_name(
                            typecheck_type_for_declaration(declared_type)));
                runtime_error(runtime, instruction->column, message);
                goto done;
            }
            save_state_to_persistence(runtime);
            depth--;
            break;
        }
        case BC_STDLIB_CALL:
        {
            if (instruction->operand < BC_STDLIB_ABS ||
                instruction->operand > BC_STDLIB_COUNT ||
                instruction->operand2 < 0 ||
                (size_t)instruction->operand2 > depth)
                goto invalid_bytecode;
            size_t argument_count = (size_t)instruction->operand2;
            size_t argument_base = depth - argument_count;
            Value result = value_null();
            char error[192] = "";
            if (!execute_stdlib_call(runtime, instruction, &stack[argument_base],
                                     argument_count,
                                     &result, error, sizeof(error)))
            {
                runtime_error_span(runtime, instruction->column, instruction->span_length,
                                   error[0] ? error : "Standard-library call failed.");
                goto done;
            }
            stack[argument_base] = result;
            depth = argument_base + 1;
            break;
        }
        case BC_CONTAINS:
        {
            if (depth < 2)
                goto invalid_bytecode;
            bool contains = false;
            bool success = stack[depth - 1].type == VALUE_MAP
                               ? value_map_contains(&stack[depth - 1], &stack[depth - 2],
                                                    &contains)
                           : stack[depth - 1].type == VALUE_SET
                               ? value_set_contains(&stack[depth - 1], &stack[depth - 2],
                                                    &contains)
                               : false;
            if (!success)
            {
                runtime_error(runtime, instruction->column,
                              "Membership requires a map or set and a valid key.");
                goto done;
            }
            stack[depth - 2] = value_bool(contains);
            depth--;
            break;
        }
        case BC_COMPARE:
        {
            if (depth < 2 || instruction->operand < VALUE_OP_EQUAL ||
                instruction->operand > VALUE_OP_GREATER_EQUAL)
                goto invalid_bytecode;
            Value result = value_null();
            char error[128];
            if (!value_binary(runtime, (ValueBinaryOp)instruction->operand,
                              &stack[depth - 2], &stack[depth - 1],
                              &result, error, sizeof(error)))
            {
                runtime_error_span(runtime, instruction->column,
                                   instruction->span_length, error);
                goto done;
            }
            stack[depth - 2] = result;
            depth--;
            break;
        }
        case BC_NEGATE:
            if (depth == 0)
                goto invalid_bytecode;
            if (stack[depth - 1].type == VALUE_FLOAT)
            {
                double negated = -stack[depth - 1].as.floating;
                if (!isfinite(negated))
                {
                    runtime_error_span(runtime, instruction->column,
                                       instruction->span_length,
                                       "Floating-point negation produced a non-finite result.");
                    goto done;
                }
                stack[depth - 1] = value_float(negated);
                break;
            }
            if (operand_integer(&stack[depth - 1]) == INT_MIN)
            {
                runtime_error(runtime, instruction->column, "Integer overflow in unary negation.");
                goto done;
            }
            stack[depth - 1] = value_int(-operand_integer(&stack[depth - 1]));
            break;
        case BC_JUMP:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->instruction_count)
                goto invalid_bytecode;
            pc = (size_t)instruction->operand;
            continue;
        case BC_JUMP_IF_FALSE:
            if (depth == 0 || instruction->operand < 0 ||
                (size_t)instruction->operand >= program->instruction_count)
                goto invalid_bytecode;
            if (!value_truthy(&stack[--depth]))
            {
                pc = (size_t)instruction->operand;
                continue;
            }
            break;
        case BC_REPEAT_INIT:
            if (depth == 0 || instruction->operand < 0 ||
                (size_t)instruction->operand >= program->instruction_count)
                goto invalid_bytecode;
            {
                int count = operand_integer(&stack[--depth]);
                if (count <= 0)
                {
                    pc = (size_t)instruction->operand;
                    continue;
                }
                if (repeat_depth == repeat_capacity &&
                    !grow_array((void **)&repeat_frames, &repeat_capacity, sizeof(*repeat_frames)))
                {
                    runtime_error(runtime, instruction->column, "Unable to allocate repeat-loop state.");
                    goto done;
                }
                repeat_frames[repeat_depth].remaining = count;
                repeat_frames[repeat_depth].body_pc = pc + 1;
                repeat_depth++;
            }
            break;
        case BC_REPEAT_NEXT:
            if (repeat_depth == 0 || instruction->operand < 0 ||
                (size_t)instruction->operand >= program->instruction_count)
                goto invalid_bytecode;
            if (--repeat_frames[repeat_depth - 1].remaining > 0)
            {
                pc = (size_t)instruction->operand;
                continue;
            }
            repeat_depth--;
            break;
        case BC_CALL:
            if (instruction->operand < 0 || (size_t)instruction->operand >= runtime->func_count ||
                instruction->operand2 < 0 || (size_t)instruction->operand2 > depth)
                goto invalid_bytecode;
            {
                Function *function = &runtime->funcs[instruction->operand];
                size_t argument_count = (size_t)instruction->operand2;
                if (argument_count != function->parameter_count || call_depth >= MAX_STACK_DEPTH ||
                    runtime->stack_top >= MAX_STACK_DEPTH - 1 || function->entry_pc >= program->instruction_count)
                {
                    runtime_error(runtime, instruction->column, "Invalid action call or call-stack limit reached.");
                    goto done;
                }
                CallFrame *frame = &call_frames[call_depth++];
                frame->return_pc = pc + 1;
                frame->stack_depth = depth - argument_count;
                frame->repeat_depth = repeat_depth;
                frame->exception_depth = exception_depth;

                int argument_base = (int)frame->stack_depth;
                runtime->stack_top++;
                if (!init_scope(&runtime->call_stack[runtime->stack_top], runtime))
                    goto done;
                for (size_t i = 0; i < argument_count; i++)
                {
                    if (!bind_argument(runtime, function->parameters[i],
                                       &stack[argument_base + (int)i]))
                    {
                        runtime_error(runtime, instruction->column,
                                      "Action parameter cannot accept this value.");
                        goto done;
                    }
                }
                depth = frame->stack_depth;
                pc = function->entry_pc;
                continue;
            }
        case BC_METHOD_CALL:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= program->symbol_count ||
                instruction->operand3 < 0 || (size_t)instruction->operand3 > depth)
                goto invalid_bytecode;
            Variable *receiver = get_var_obj(runtime, program->symbols[instruction->operand]);
            if (!receiver || !receiver->has_runtime_value ||
                receiver->runtime_value.type != VALUE_OBJECT)
            {
                runtime_error(runtime, instruction->column, "Method receiver is not an object.");
                goto done;
            }
            size_t class_id = value_object_class_id(&receiver->runtime_value);
            if (class_id >= runtime->class_count)
            {
                runtime_error(runtime, instruction->column, "Method receiver has an invalid class.");
                goto done;
            }
            int function_index = find_class_method(runtime, (int)class_id,
                                                   program->symbols[instruction->operand2]);
            if (function_index < 0)
            {
                runtime_error(runtime, instruction->column, "Object method is not defined.");
                goto done;
            }
            Function *function = &runtime->funcs[function_index];
            size_t argument_count = (size_t)instruction->operand3;
            if (argument_count != function->parameter_count ||
                call_depth >= MAX_STACK_DEPTH || runtime->stack_top >= MAX_STACK_DEPTH - 1 ||
                function->entry_pc >= program->instruction_count)
            {
                runtime_error(runtime, instruction->column, "Invalid method call or call-stack limit reached.");
                goto done;
            }
            CallFrame *frame = &call_frames[call_depth++];
            frame->return_pc = pc + 1;
            frame->stack_depth = depth - argument_count;
            frame->repeat_depth = repeat_depth;
            frame->exception_depth = exception_depth;
            Value receiver_value = value_null();
            if (!value_copy(&receiver_value, &receiver->runtime_value))
            {
                runtime_error(runtime, instruction->column, "Unable to retain method receiver.");
                goto done;
            }
            int argument_base = (int)frame->stack_depth;
            runtime->stack_top++;
            if (!init_scope(&runtime->call_stack[runtime->stack_top], runtime))
            {
                value_release(&receiver_value);
                goto done;
            }
            Variable *this_variable = get_or_create_var(runtime, "this");
            bool bound_this = variable_replace_runtime_value(this_variable,
                                                             &receiver_value);
            value_release(&receiver_value);
            if (!bound_this)
            {
                runtime_error(runtime, instruction->column, "Unable to bind method receiver.");
                goto done;
            }
            for (size_t i = 0; i < argument_count; i++)
            {
                if (!bind_argument(runtime, function->parameters[i],
                                   &stack[argument_base + (int)i]))
                {
                    runtime_error(runtime, instruction->column,
                                  "Method parameter cannot accept this value.");
                    goto done;
                }
            }
            depth = frame->stack_depth;
            pc = function->entry_pc;
            continue;
        }
        case BC_NEW:
        {
            if (instruction->operand < 0 || instruction->operand2 < 0 ||
                (size_t)instruction->operand >= program->symbol_count ||
                (size_t)instruction->operand2 >= runtime->class_count ||
                instruction->operand3 < 0 || (size_t)instruction->operand3 > depth)
                goto invalid_bytecode;
            int class_index = instruction->operand2;
            int constructor = find_class_method(runtime, class_index, "init");
            size_t argument_count = (size_t)instruction->operand3;
            if ((constructor >= 0 && runtime->funcs[constructor].parameter_count != argument_count) ||
                (constructor < 0 && argument_count != 0))
            {
                runtime_error(runtime, instruction->column, "Constructor argument count does not match init.");
                goto done;
            }
            Value instance = value_null();
            if (!runtime_object_create(runtime, (size_t)class_index, &instance))
            {
                runtime_error(runtime, instruction->column, "Unable to allocate object instance.");
                goto done;
            }
            if (!value_root_add(runtime, &instance))
            {
                value_release(&instance);
                runtime_error(runtime, instruction->column, "Unable to root new object instance.");
                goto done;
            }
            Variable *target = get_or_create_var(runtime, program->symbols[instruction->operand]);
            bool stored_instance = variable_replace_runtime_value(target, &instance);
            if (!value_root_remove(runtime, &instance))
                stored_instance = false;
            value_release(&instance);
            if (!stored_instance)
            {
                runtime_error(runtime, instruction->column, "Unable to store new object instance.");
                goto done;
            }
            if (constructor >= 0)
            {
                if (call_depth >= MAX_STACK_DEPTH || runtime->stack_top >= MAX_STACK_DEPTH - 1 ||
                    runtime->funcs[constructor].entry_pc >= program->instruction_count)
                {
                    runtime_error(runtime, instruction->column, "Invalid constructor or call-stack limit reached.");
                    goto done;
                }
                CallFrame *frame = &call_frames[call_depth++];
                frame->return_pc = pc + 1;
                frame->stack_depth = depth - argument_count;
                frame->repeat_depth = repeat_depth;
                frame->exception_depth = exception_depth;
                int argument_base = (int)frame->stack_depth;
                runtime->stack_top++;
                if (!init_scope(&runtime->call_stack[runtime->stack_top], runtime))
                    goto done;
                Variable *this_variable = get_or_create_var(runtime, "this");
                Variable *constructed = get_var_obj(runtime, program->symbols[instruction->operand]);
                if (!constructed || !constructed->has_runtime_value ||
                    !variable_replace_runtime_value(this_variable,
                                                    &constructed->runtime_value))
                {
                    runtime_error(runtime, instruction->column, "Unable to bind constructor receiver.");
                    goto done;
                }
                for (size_t i = 0; i < argument_count; i++)
                {
                    if (!bind_argument(runtime, runtime->funcs[constructor].parameters[i],
                                       &stack[argument_base + (int)i]))
                    {
                        runtime_error(runtime, instruction->column,
                                      "Constructor parameter cannot accept this value.");
                        goto done;
                    }
                }
                depth = frame->stack_depth;
                pc = runtime->funcs[constructor].entry_pc;
                continue;
            }
            depth -= argument_count;
            break;
        }
        case BC_RETURN:
            if (call_depth == 0 || runtime->stack_top <= initial_stack_top)
                goto invalid_bytecode;
            {
                CallFrame frame = call_frames[--call_depth];
                free_scope(&runtime->call_stack[runtime->stack_top]);
                runtime->stack_top--;
                depth = frame.stack_depth;
                repeat_depth = frame.repeat_depth;
                exception_depth = frame.exception_depth;
                pc = frame.return_pc;
                continue;
            }
        case BC_TRY:
            if (instruction->operand < 0 ||
                (size_t)instruction->operand >= program->instruction_count ||
                instruction->operand2 < 0 ||
                (size_t)instruction->operand2 >= program->symbol_count ||
                exception_depth >= MAX_STACK_DEPTH)
                goto invalid_bytecode;
            exception_frames[exception_depth].catch_pc = (size_t)instruction->operand;
            exception_frames[exception_depth].stack_depth = depth;
            exception_frames[exception_depth].repeat_depth = repeat_depth;
            exception_frames[exception_depth].call_depth = call_depth;
            exception_frames[exception_depth].scope_top = runtime->stack_top;
            exception_frames[exception_depth].exception_symbol = instruction->operand2;
            exception_depth++;
            break;
        case BC_END_TRY:
            if (exception_depth == 0)
                goto invalid_bytecode;
            exception_depth--;
            break;
        case BC_THROW:
            if (depth == 0)
                goto invalid_bytecode;
            if (exception_depth == 0)
            {
                runtime_error(runtime, instruction->column, "Uncaught integer exception.");
                goto done;
            }
            {
                int exception_value = operand_integer(&stack[--depth]);
                ExceptionFrame frame = exception_frames[--exception_depth];
                while (runtime->stack_top > frame.scope_top)
                {
                    free_scope(&runtime->call_stack[runtime->stack_top]);
                    runtime->stack_top--;
                }
                call_depth = frame.call_depth;
                repeat_depth = frame.repeat_depth;
                depth = frame.stack_depth;
                Variable *exception_variable =
                    get_or_create_var(runtime, program->symbols[frame.exception_symbol]);
                if (!variable_replace_runtime_value(exception_variable,
                                                    &(Value){.type = VALUE_INT,
                                                             .as.integer = exception_value}))
                {
                    runtime_error(runtime, instruction->column,
                                  "Unable to bind caught exception value.");
                    goto done;
                }
                pc = frame.catch_pc;
                continue;
            }
        case BC_ADD:
        case BC_SUB:
        case BC_MUL:
        case BC_DIV:
        {
            if (depth < 2)
                goto invalid_bytecode;
            if (stack[depth - 2].type == VALUE_FLOAT ||
                stack[depth - 1].type == VALUE_FLOAT)
            {
                Value result = value_null();
                char error[192] = "";
                ValueBinaryOp operation =
                    instruction->opcode == BC_ADD ? VALUE_OP_ADD
                    : instruction->opcode == BC_SUB ? VALUE_OP_SUBTRACT
                    : instruction->opcode == BC_MUL ? VALUE_OP_MULTIPLY
                                                    : VALUE_OP_DIVIDE;
                if (!value_binary(runtime, operation, &stack[depth - 2],
                                  &stack[depth - 1], &result, error,
                                  sizeof(error)))
                {
                    runtime_error_span(
                        runtime, instruction->column, instruction->span_length,
                        error[0] ? error : "Floating-point arithmetic failed.");
                    goto done;
                }
                stack[depth - 2] = result;
                depth--;
                break;
            }
            int left = operand_integer(&stack[depth - 2]);
            int right = operand_integer(&stack[depth - 1]);
            if (instruction->opcode == BC_DIV && right == 0)
            {
                if (exception_depth == 0)
                {
                    report_diagnostic(runtime, "warning", "W2001", "Division by zero; left operand preserved.",
                                      runtime->current_line, instruction->column);
                    depth--;
                    break;
                }
                ExceptionFrame frame = exception_frames[--exception_depth];
                while (runtime->stack_top > frame.scope_top)
                {
                    free_scope(&runtime->call_stack[runtime->stack_top]);
                    runtime->stack_top--;
                }
                call_depth = frame.call_depth;
                repeat_depth = frame.repeat_depth;
                depth = frame.stack_depth;
                Variable *exception_variable =
                    get_or_create_var(runtime, program->symbols[frame.exception_symbol]);
                if (!variable_replace_runtime_value(exception_variable,
                                                    &(Value){.type = VALUE_INT,
                                                             .as.integer = 1}))
                {
                    runtime_error(runtime, instruction->column,
                                  "Unable to bind caught exception value.");
                    goto done;
                }
                pc = frame.catch_pc;
                continue;
            }
            if (instruction->opcode == BC_DIV && left == INT_MIN && right == -1)
            {
                runtime_error(runtime, instruction->column, "Integer overflow in division.");
                goto done;
            }
            long long value = instruction->opcode == BC_ADD   ? (long long)left + right
                              : instruction->opcode == BC_SUB ? (long long)left - right
                              : instruction->opcode == BC_MUL ? (long long)left * right
                                                              : left / right;
            if (value < INT_MIN || value > INT_MAX)
            {
                runtime_error(runtime, instruction->column, "Integer overflow in arithmetic expression.");
                goto done;
            }
            stack[depth - 2] = value_int((int)value);
            depth--;
            break;
        }
        case BC_HALT:
            if (call_depth != 0 || repeat_depth != 0 || exception_depth != 0 ||
                depth != (program->has_result ? 1U : 0U))
                goto invalid_bytecode;
            if (program->has_result)
            {
                if (value_result)
                {
                    if (!value_copy(value_result, &stack[0]))
                    {
                        runtime_error(runtime, instruction->column, "Unable to return expression value.");
                        goto done;
                    }
                }
                else if (integer_result)
                {
                    *integer_result = stack[0].type == VALUE_BOOL
                                          ? (stack[0].as.boolean ? 1 : 0)
                                          : operand_integer(&stack[0]);
                }
            }
            success = true;
            goto done;
        default:
            goto invalid_bytecode;
        }
        pc++;
    }

invalid_bytecode:
    runtime_error(runtime, 1, "Invalid bytecode or stack state.");
done:
    while (runtime->stack_top > initial_stack_top)
    {
        free_scope(&runtime->call_stack[runtime->stack_top]);
        runtime->stack_top--;
    }
    free(repeat_frames);
    if (!gc_root_remove(runtime, &stack_root))
    {
        runtime_error(runtime, 1, "VM operand stack was not registered as a GC root.");
        success = false;
    }
    free(stack);
    return success;
}

bool bytecode_execute(RuntimeContext *runtime, const BytecodeProgram *program,
                      int *result)
{
    return bytecode_execute_internal(runtime, program, result, NULL);
}

bool bytecode_execute_value(RuntimeContext *runtime, const BytecodeProgram *program,
                            Value *result)
{
    return bytecode_execute_internal(runtime, program, NULL, result);
}

bool bytecode_evaluate_expression(RuntimeContext *runtime, const char *source,
                                  int *result)
{
    if (!runtime_is_initialized(runtime))
        return false;
    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    bool success = bytecode_compile_expression(runtime, source, &program);
    if (success)
        success = bytecode_execute(runtime, &program, result);
    bytecode_free(&program);
    return success;
}

bool bytecode_evaluate_value(RuntimeContext *runtime, const char *source,
                             Value *result)
{
    if (!runtime_is_initialized(runtime) || !result)
        return false;
    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    bool success = bytecode_compile_expression(runtime, source, &program);
    if (success)
        success = bytecode_execute_value(runtime, &program, result);
    bytecode_free(&program);
    return success;
}