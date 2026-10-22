#include "../main.h"
#include "../bytecode.h"
#include "../gc.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>

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

static bool set_source(RuntimeContext *runtime, const char *source)
{
    runtime_clear_source(runtime);
    while (*source)
    {
        char line[MAX_LINE_LENGTH];
        size_t length = 0;
        while (source[length] && source[length] != '\n' &&
               length < sizeof(line) - 2)
            length++;
        memcpy(line, source, length);
        if (source[length] == '\n')
        {
            line[length++] = '\n';
            source += length;
        }
        else
            source += length;
        line[length] = '\0';
        if (!append_line(runtime, line))
            return false;
    }
    return runtime->source_line_count != 0;
}

static bool compile_and_execute(RuntimeContext *runtime, const char *source,
                                bool compile_success, bool execute_success)
{
    if (!set_source(runtime, source))
        return false;
    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    bool compiled = bytecode_compile_program(
        runtime, 0, (int)runtime->source_line_count - 1, &program);
    if (compiled != compile_success)
    {
        bytecode_free(&program);
        return false;
    }
    if (compiled)
    {
        bool executed = bytecode_execute(runtime, &program, NULL);
        bytecode_free(&program);
        if (executed != execute_success)
            return false;
    }
    return true;
}

static Variable *variable(RuntimeContext *runtime, const char *name)
{
    return get_var_obj(runtime, name);
}

static int test_declarations_and_dynamic_behavior(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));

    Value floating = value_float(1.5);
    Variable *float_source = get_or_create_var(&runtime, "float_source");
    CHECK(float_source != NULL);
    CHECK(variable_replace_runtime_value(float_source, &floating));

    CHECK(compile_and_execute(
        &runtime,
        "integer_value: Int = 2 + 3\n"
        "float_value: Float = float_source\n"
        "boolean_value: Bool = true\n"
        "string_value: String = \"hello\"\n"
        "null_value: Null = null\n"
        "dynamic_value = true\n"
        "dynamic_value = 7\n",
        true, true));
    CHECK(variable(&runtime, "integer_value")->declared_type == VARIABLE_TYPE_INT);
    CHECK(variable(&runtime, "integer_value")->runtime_value.type == VALUE_INT);
    CHECK(variable(&runtime, "integer_value")->runtime_value.as.integer == 5);
    CHECK(variable(&runtime, "float_value")->declared_type == VARIABLE_TYPE_FLOAT);
    CHECK(variable(&runtime, "float_value")->runtime_value.type == VALUE_FLOAT);
    CHECK(variable(&runtime, "boolean_value")->declared_type == VARIABLE_TYPE_BOOL);
    CHECK(variable(&runtime, "string_value")->declared_type == VARIABLE_TYPE_STRING);
    CHECK(variable(&runtime, "null_value")->declared_type == VARIABLE_TYPE_NULL);
    CHECK(variable(&runtime, "dynamic_value")->declared_type ==
          VARIABLE_TYPE_UNDECLARED);
    CHECK(variable(&runtime, "dynamic_value")->runtime_value.type == VALUE_INT);

    CHECK(compile_and_execute(&runtime, "bad: Int = \"wrong\"\n", false, false));
    CHECK(compile_and_execute(&runtime, "bad_operation: Int = true + 1\n",
                              false, false));
    CHECK(compile_and_execute(&runtime, "division_by_zero: Int = 1 / 0\n",
                              false, false));
    runtime_destroy(&runtime);
    return 0;
}

static int test_dynamic_and_reassignment_enforcement(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));

    Value text = value_null();
    CHECK(value_string(&runtime, "not an integer", 14, &text));
    Variable *source = get_or_create_var(&runtime, "unknown_source");
    CHECK(source != NULL);
    CHECK(variable_replace_runtime_value(source, &text));
    value_release(&text);
    CHECK(compile_and_execute(&runtime, "value: Int = unknown_source\n",
                              true, false));
    CHECK(variable(&runtime, "value")->declared_type ==
          VARIABLE_TYPE_UNDECLARED);

    CHECK(compile_and_execute(&runtime,
                              "value: Int = 3\nvalue = \"wrong\"\n",
                              true, false));
    CHECK(variable(&runtime, "value")->declared_type == VARIABLE_TYPE_INT);
    CHECK(variable(&runtime, "value")->runtime_value.type == VALUE_INT);
    CHECK(variable(&runtime, "value")->runtime_value.as.integer == 3);
    runtime_destroy(&runtime);
    return 0;
}

static int test_replacement_operation_enforcement(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));

    CHECK(compile_and_execute(&runtime, "value: Int = 9\nlist value\n",
                              true, false));
    CHECK(variable(&runtime, "value")->runtime_value.type == VALUE_INT);
    CHECK(variable(&runtime, "value")->runtime_value.as.integer == 9);
    runtime_destroy(&runtime);
    return 0;
}

static int test_context_isolation(void)
{
    RuntimeContext first = RUNTIME_CONTEXT_INIT;
    RuntimeContext second = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&first));
    CHECK(runtime_init(&second));
    first.stack_top = 0;
    second.stack_top = 0;
    CHECK(init_scope(&first.call_stack[0], &first));
    CHECK(init_scope(&second.call_stack[0], &second));

    CHECK(compile_and_execute(&first, "same_name: Int = 12\n", true, true));
    CHECK(compile_and_execute(&second, "same_name: String = \"separate\"\n",
                              true, true));
    CHECK(variable(&first, "same_name")->declared_type == VARIABLE_TYPE_INT);
    CHECK(variable(&second, "same_name")->declared_type == VARIABLE_TYPE_STRING);
    CHECK(variable(&first, "same_name")->runtime_value.type == VALUE_INT);
    CHECK(variable(&second, "same_name")->runtime_value.type == VALUE_STRING);
    runtime_destroy(&first);
    runtime_destroy(&second);
    return 0;
}

static int test_persistence_and_gc(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));
    CHECK(set_source(&runtime, "retained: String = \"managed value\"\n"));
    (void)snprintf(runtime.source_filename, sizeof(runtime.source_filename),
                   "tests\\type_annotations_state_fixture");
    {
        BytecodeProgram program = BYTECODE_PROGRAM_INIT;
        CHECK(bytecode_compile_program(&runtime, 0,
                                       (int)runtime.source_line_count - 1,
                                       &program));
        CHECK(bytecode_execute(&runtime, &program, NULL));
        bytecode_free(&program);
    }
    Variable *retained = variable(&runtime, "retained");
    CHECK(retained != NULL);
    CHECK(retained->declared_type == VARIABLE_TYPE_STRING);
    CHECK(gc_get_stats(&runtime).object_count == 1);
    gc_collect(&runtime);
    CHECK(gc_get_stats(&runtime).object_count == 1);
    CHECK(retained->declared_type == VARIABLE_TYPE_STRING);

    load_state_from_persistence(&runtime);
    retained = variable(&runtime, "retained");
    CHECK(retained != NULL);
    CHECK(retained->declared_type == VARIABLE_TYPE_STRING);
    CHECK(retained->runtime_value.type == VALUE_STRING);
    {
        size_t length = 0;
        const char *bytes = value_string_bytes(&retained->runtime_value, &length);
        CHECK(bytes != NULL);
        CHECK(length == strlen("managed value"));
        CHECK(memcmp(bytes, "managed value", length) == 0);
    }
    runtime_destroy(&runtime);
    CHECK(remove("tests\\type_annotations_state_fixture.state") == 0);
    return 0;
}

int main(void)
{
    CHECK(test_declarations_and_dynamic_behavior() == 0);
    CHECK(test_dynamic_and_reassignment_enforcement() == 0);
    CHECK(test_replacement_operation_enforcement() == 0);
    CHECK(test_context_isolation() == 0);
    CHECK(test_persistence_and_gc() == 0);
    puts("Optional static type tests passed");
    return 0;
}
