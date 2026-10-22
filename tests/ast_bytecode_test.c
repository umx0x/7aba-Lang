#include "../main.h"
#include "../bytecode.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define CHECK(condition)                                      \
    do                                                        \
    {                                                         \
        if (!(condition))                                     \
        {                                                     \
            fprintf(stderr, "check failed at line %d: %s\n", \
                    __LINE__, #condition);                    \
            return 1;                                         \
        }                                                     \
    } while (0)

static int run_scalar_pair(RuntimeContext *runtime, const char *source,
                           ValueType expected_type, int64_t expected_integer,
                           bool expected_boolean, const char *expected_string)
{
    BytecodeProgram ast_program = BYTECODE_PROGRAM_INIT;
    BytecodeProgram legacy_program = BYTECODE_PROGRAM_INIT;
    Value ast_value = value_null();
    Value legacy_value = value_null();
    bool ast_compiled =
        bytecode_compile_ast_expression(runtime, source, &ast_program);
    bool legacy_compiled =
        bytecode_compile_expression(runtime, source, &legacy_program);
    bool ast_executed = ast_compiled &&
                        bytecode_execute_value(runtime, &ast_program,
                                               &ast_value);
    bool legacy_executed = legacy_compiled &&
                           bytecode_execute_value(runtime, &legacy_program,
                                                  &legacy_value);
    if (!ast_compiled || !legacy_compiled || !ast_executed || !legacy_executed)
    {
        fprintf(stderr,
                "expression failed: %s (AST compile=%d execute=%d, legacy compile=%d execute=%d)\n",
                source, ast_compiled, ast_executed, legacy_compiled,
                legacy_executed);
        bytecode_free(&ast_program);
        bytecode_free(&legacy_program);
        value_release(&ast_value);
        value_release(&legacy_value);
        return 0;
    }

    int matches = ast_value.type == expected_type &&
                  legacy_value.type == expected_type;
    if (matches && expected_type == VALUE_INT)
        matches = ast_value.as.integer == expected_integer &&
                  legacy_value.as.integer == expected_integer;
    else if (matches && expected_type == VALUE_BOOL)
        matches = ast_value.as.boolean == expected_boolean &&
                  legacy_value.as.boolean == expected_boolean;
    else if (matches && expected_type == VALUE_STRING)
    {
        size_t ast_length = 0;
        size_t legacy_length = 0;
        const char *ast_text = value_string_bytes(&ast_value, &ast_length);
        const char *legacy_text = value_string_bytes(&legacy_value, &legacy_length);
        size_t expected_length = strlen(expected_string);
        matches = ast_text && legacy_text &&
                  ast_length == expected_length && legacy_length == expected_length &&
                  memcmp(ast_text, expected_string, expected_length) == 0 &&
                  memcmp(legacy_text, expected_string, expected_length) == 0;
    }
    if (!matches)
        fprintf(stderr, "value mismatch for %s: AST type=%d legacy type=%d expected=%d\n",
                source, (int)ast_value.type, (int)legacy_value.type,
                (int)expected_type);
    value_release(&ast_value);
    value_release(&legacy_value);
    bytecode_free(&ast_program);
    bytecode_free(&legacy_program);
    return matches;
}

static int run_float_pair(RuntimeContext *runtime, const char *source,
                          double expected)
{
    BytecodeProgram ast_program = BYTECODE_PROGRAM_INIT;
    BytecodeProgram legacy_program = BYTECODE_PROGRAM_INIT;
    Value ast_value = value_null();
    Value legacy_value = value_null();
    bool success =
        bytecode_compile_ast_expression(runtime, source, &ast_program) &&
        bytecode_compile_expression(runtime, source, &legacy_program) &&
        bytecode_execute_value(runtime, &ast_program, &ast_value) &&
        bytecode_execute_value(runtime, &legacy_program, &legacy_value);
    bool matches = success && ast_value.type == VALUE_FLOAT &&
                   legacy_value.type == VALUE_FLOAT &&
                   fabs(ast_value.as.floating - expected) < 0.000001 &&
                   fabs(legacy_value.as.floating - expected) < 0.000001;
    if (!matches)
        fprintf(stderr, "float result mismatch for %s\n", source);
    value_release(&ast_value);
    value_release(&legacy_value);
    bytecode_free(&ast_program);
    bytecode_free(&legacy_program);
    return matches;
}

static bool has_opcode(const BytecodeProgram *program, BytecodeOpcode opcode)
{
    for (size_t i = 0; i < program->instruction_count; i++)
        if (program->instructions[i].opcode == opcode)
            return true;
    return false;
}

static int load_fixture(RuntimeContext *runtime, const char *path)
{
    FILE *fixture = fopen(path, "r");
    if (!fixture)
        return 0;
    runtime_clear_source(runtime);
    char line[MAX_LINE_LENGTH];
    int success = 1;
    while (fgets(line, sizeof(line), fixture))
        if (!append_line(runtime, line))
        {
            success = 0;
            break;
        }
    if (ferror(fixture))
        success = 0;
    fclose(fixture);
    return success;
}

static int check_parser_parity(RuntimeContext *runtime)
{
    static const struct
    {
        const char *source;
        bool accepted;
    } cases[] = {
        {"1 + 2 * 3", true},
        {"abs(1) + max(2, 3)", true},
        {"trim(\" text \")", true},
        {"starts_with(\"text\", \"te\")", true},
        {"ends_with(\"text\", \"xt\")", true},
        {"contains(\"text\", \"ex\")", true},
        {"index_of(\"text\", \"xt\")", true},
        {"count(\"banana\", \"an\")", true},
        {"replace(\"text\", \"e\", \"a\")", true},
        {"repeat(\"a\", 3)", true},
        {"starts_with(\"text\")", false},
        {"sqrt(9)", true},
        {"pi()", true},
        {"random_int(-2, 2)", true},
        {"random_seed(10)", true},
        {"pi(1)", false},
        {"abs(1,)", false},
        {"abs(1, 2,)", false},
        {"{\"a\": 1,}", true},
        {"set{1, 2,}", true},
        {"value[0]", true},
        {"value . field", true},
        {"value[0] {", true},
        {"value[0] { # block opener", true},
        {"value[0][1]", false},
        {"value.field[0]", false},
        {"value[0].field", false},
        {"(value)[0]", false},
        {"\"value\"[0]", false},
        {"true.field", false},
        {"unknown()", false},
        {"1 + # comment", false},
        {"1 + 2 # comment", true},
        {"1 + 2 {", true},
        {"1 + 2 {{", false}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        BytecodeProgram ast_program = BYTECODE_PROGRAM_INIT;
        BytecodeProgram production_program = BYTECODE_PROGRAM_INIT;
        bool ast_ok = bytecode_compile_ast_expression(runtime, cases[i].source,
                                                      &ast_program);
        bool production_ok = bytecode_compile_expression(runtime, cases[i].source,
                                                         &production_program);
        bool matches = ast_ok == cases[i].accepted &&
                       production_ok == cases[i].accepted &&
                       ast_ok == production_ok;
        if (!matches)
            fprintf(stderr, "parser parity mismatch for \"%s\": AST=%d production=%d expected=%d\n",
                    cases[i].source, ast_ok, production_ok, cases[i].accepted);
        bytecode_free(&ast_program);
        bytecode_free(&production_program);
        if (!matches)
            return 0;
    }
    return 1;
}

static bool evaluate_value(RuntimeContext *runtime, const char *source,
                           Value *result)
{
    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    if (!bytecode_compile_expression(runtime, source, &program))
    {
        bytecode_free(&program);
        return false;
    }
    bool success = bytecode_execute_value(runtime, &program, result);
    bytecode_free(&program);
    return success;
}

static bool evaluation_fails(RuntimeContext *runtime, const char *source)
{
    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    Value result = value_null();
    bool compiled = bytecode_compile_expression(runtime, source, &program);
    bool executed = compiled &&
                    bytecode_execute_value(runtime, &program, &result);
    value_release(&result);
    bytecode_free(&program);
    return compiled && !executed;
}

int main(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.current_line = 1;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));
    CHECK(check_parser_parity(&runtime));

    CHECK(run_scalar_pair(&runtime, "1 + 2 * -3 == -5",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "\"alpha\" < \"beta\"",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "abs(-17) + max(2, 3)",
                          VALUE_INT, 20, false, NULL));
    CHECK(run_scalar_pair(&runtime, "2 in {1, 2, 3}",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "len({\"a\": 4, \"b\": 5})",
                          VALUE_INT, 2, false, NULL));
    CHECK(run_scalar_pair(&runtime, "len(set{1, 2, 2})",
                          VALUE_INT, 2, false, NULL));
    CHECK(run_scalar_pair(&runtime, "trim(\" \\t hello \\n\")",
                          VALUE_STRING, 0, false, "hello"));
    CHECK(run_scalar_pair(&runtime, "starts_with(\"hello\", \"he\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "starts_with(\"hello\", \"lo\")",
                          VALUE_BOOL, 0, false, NULL));
    CHECK(run_scalar_pair(&runtime, "ends_with(\"hello\", \"lo\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "ends_with(\"hello\", \"he\")",
                          VALUE_BOOL, 0, false, NULL));
    CHECK(run_scalar_pair(&runtime, "starts_with(\"\", \"\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "ends_with(\"text\", \"\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "contains(\"banana\", \"ana\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "contains(\"banana\", \"xyz\")",
                          VALUE_BOOL, 0, false, NULL));
    CHECK(run_scalar_pair(&runtime, "contains(\"text\", \"\")",
                          VALUE_BOOL, 0, true, NULL));
    CHECK(run_scalar_pair(&runtime, "index_of(\"banana\", \"ana\")",
                          VALUE_INT, 1, false, NULL));
    CHECK(run_scalar_pair(&runtime, "index_of(\"banana\", \"xyz\")",
                          VALUE_INT, -1, false, NULL));
    CHECK(run_scalar_pair(&runtime, "index_of(\"text\", \"\")",
                          VALUE_INT, 0, false, NULL));
    CHECK(run_scalar_pair(&runtime, "count(\"banana\", \"an\")",
                          VALUE_INT, 2, false, NULL));
    CHECK(run_scalar_pair(&runtime, "count(\"banana\", \"na\")",
                          VALUE_INT, 2, false, NULL));
    CHECK(run_scalar_pair(&runtime, "count(\"text\", \"\")",
                          VALUE_INT, 5, false, NULL));
    CHECK(run_scalar_pair(&runtime, "replace(\"one one\", \"one\", \"two\")",
                          VALUE_STRING, 0, false, "two two"));
    CHECK(run_scalar_pair(&runtime, "replace(\"aaaa\", \"aa\", \"b\")",
                          VALUE_STRING, 0, false, "bb"));
    CHECK(run_scalar_pair(&runtime, "replace(\"text\", \"\", \"x\")",
                          VALUE_STRING, 0, false, "text"));
    CHECK(run_scalar_pair(&runtime, "repeat(\"ab\", 3)",
                          VALUE_STRING, 0, false, "ababab"));
    CHECK(run_scalar_pair(&runtime, "repeat(\"ab\", 0)",
                          VALUE_STRING, 0, false, ""));
    CHECK(evaluation_fails(&runtime, "repeat(\"x\", -1)"));
    CHECK(run_scalar_pair(&runtime, "trim(\" \\t\\n\")",
                          VALUE_STRING, 0, false, ""));
    CHECK(run_scalar_pair(&runtime, "random_int(5, 5)",
                          VALUE_INT, 5, false, NULL));
    CHECK(run_scalar_pair(&runtime, "random_seed(123)", VALUE_NULL, 0,
                          false, NULL));
    CHECK(run_float_pair(&runtime, "sqrt(9) + 1", 4.0));
    CHECK(run_float_pair(&runtime, "-cos(0)", -1.0));

    Value float_result = value_null();
    CHECK(evaluate_value(&runtime, "sqrt(9)", &float_result));
    CHECK(float_result.type == VALUE_FLOAT &&
          fabs(float_result.as.floating - 3.0) < 0.000001);
    value_release(&float_result);
    CHECK(evaluate_value(&runtime, "pi()", &float_result));
    CHECK(float_result.type == VALUE_FLOAT &&
          fabs(float_result.as.floating - 3.14159265358979323846) < 0.000001);
    value_release(&float_result);
    CHECK(evaluate_value(&runtime, "random_seed(12345)", &float_result));
    CHECK(float_result.type == VALUE_NULL);
    value_release(&float_result);
    Value random_first = value_null();
    CHECK(evaluate_value(&runtime, "random_int(-100, 100)", &random_first));
    CHECK(random_first.type == VALUE_INT &&
          random_first.as.integer >= -100 &&
          random_first.as.integer <= 100);
    CHECK(evaluate_value(&runtime, "random_seed(12345)", &float_result));
    value_release(&float_result);
    Value random_second = value_null();
    CHECK(evaluate_value(&runtime, "random_int(-100, 100)", &random_second));
    CHECK(random_second.type == VALUE_INT &&
          random_second.as.integer == random_first.as.integer);
    value_release(&random_first);
    value_release(&random_second);
    CHECK(evaluation_fails(&runtime, "sqrt(-1)"));
    CHECK(evaluation_fails(&runtime, "log(0)"));
    CHECK(evaluation_fails(&runtime, "random_int(2, 1)"));

    Value values = value_null();
    CHECK(value_array(&runtime, &values));
    CHECK(value_array_push(&values, &(Value){.type = VALUE_INT,
                                             .as.integer = 42}));
    Variable *variable = get_or_create_var(&runtime, "values");
    CHECK(variable != NULL);
    variable->runtime_value = values;
    variable->has_runtime_value = 1;
    CHECK(value_root_add(&runtime, &variable->runtime_value));
    CHECK(run_scalar_pair(&runtime, "values[0] + 1",
                          VALUE_INT, 43, false, NULL));

    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    CHECK(bytecode_compile_expression(&runtime, "abs(2) + values[0]", &program));
    CHECK(program.has_result && program.instruction_count == 6);
    CHECK(program.instructions[0].opcode == BC_PUSH_INT);
    CHECK(program.instructions[1].opcode == BC_STDLIB_CALL);
    CHECK(program.instructions[1].column == 1 &&
          program.instructions[1].span_length == 3);
    CHECK(program.instructions[2].opcode == BC_PUSH_INT);
    CHECK(program.instructions[3].opcode == BC_LIST_GET);
    if (program.instructions[3].column != 10 ||
        program.instructions[3].span_length != 9)
        fprintf(stderr, "AST index span: %zu:%zu\n",
                program.instructions[3].column,
                program.instructions[3].span_length);
    CHECK(program.instructions[3].column == 10 &&
          program.instructions[3].span_length == 9);
    CHECK(program.instructions[4].opcode == BC_ADD);
    CHECK(program.instructions[5].opcode == BC_HALT);
    bytecode_free(&program);

    CHECK(bytecode_compile_ast_expression(&runtime, "1 in {1, 2}", &program));
    CHECK(has_opcode(&program, BC_CONTAINS));
    CHECK(has_opcode(&program, BC_BUILD_SET));
    bytecode_free(&program);

    CHECK(bytecode_compile_ast_expression(&runtime,
                                          "len({\"a\": 1}) + substr(\"xyz\", 1, 1)",
                                          &program));
    CHECK(has_opcode(&program, BC_BUILD_MAP));
    CHECK(has_opcode(&program, BC_STDLIB_CALL));
    bytecode_free(&program);
    CHECK(bytecode_compile_ast_expression(&runtime, "object.field", &program));
    CHECK(has_opcode(&program, BC_LOAD_FIELD));
    bytecode_free(&program);

    CHECK(bytecode_compile_expression(&runtime, "values[0] {", &program));
    bytecode_free(&program);
    CHECK(bytecode_compile_ast_expression(&runtime, "values[0] {", &program));
    bytecode_free(&program);
    CHECK(!bytecode_compile_expression(&runtime, "abs(2,)", &program));
    bytecode_free(&program);
    CHECK(!bytecode_compile_ast_expression(&runtime, "values[0][1]", &program));
    bytecode_free(&program);

    CHECK(load_fixture(&runtime, "tests/vm_expression.7ba"));
    CHECK(bytecode_compile_program(&runtime, 0,
                                   (int)runtime.source_line_count - 1, &program));
    CHECK(has_opcode(&program, BC_LIST_GET));
    CHECK(has_opcode(&program, BC_COMPARE));
    CHECK(has_opcode(&program, BC_STDLIB_CALL));
    CHECK(has_opcode(&program, BC_JUMP_IF_FALSE));
    {
        bool found_fixture_index_span = false;
        for (size_t i = 0; i < program.instruction_count; i++)
        {
            if (program.instructions[i].opcode == BC_LIST_GET)
            {
                if (program.instructions[i].line == 5 &&
                    program.instructions[i].column == 11 &&
                    program.instructions[i].span_length == 13)
                    found_fixture_index_span = true;
            }
        }
        CHECK(found_fixture_index_span);
    }
    bytecode_free(&program);

    CHECK(value_root_remove(&runtime, &variable->runtime_value));
    value_release(&variable->runtime_value);
    runtime_destroy(&runtime);
    puts("AST bytecode lowering tests passed");
    return 0;
}
