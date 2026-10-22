#include "../bytecode.h"
#include "../main.h"

#include <stdio.h>
#include <stdlib.h>
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

int main(void)
{
    RuntimeContext runtime = RUNTIME_CONTEXT_INIT;
    CHECK(runtime_init(&runtime));
    runtime.stack_top = 0;
    CHECK(init_scope(&runtime.call_stack[0], &runtime));

    size_t literal_length = 1024;
    char *source = malloc(literal_length + 3);
    CHECK(source != NULL);
    source[0] = '"';
    memset(source + 1, 'g', literal_length);
    source[literal_length + 1] = '"';
    source[literal_length + 2] = '\0';

    BytecodeProgram program = BYTECODE_PROGRAM_INIT;
    CHECK(bytecode_compile_expression(&runtime, source, &program));
    free(source);

    for (size_t i = 0; i < 256; i++)
    {
        Value result = value_null();
        CHECK(bytecode_execute_value(&runtime, &program, &result));
        CHECK(result.type == VALUE_STRING);
        value_release(&result);
    }

    GCStats stats = gc_get_stats(&runtime);
    CHECK(stats.collected_objects > 0);
    CHECK(stats.object_count < 128);
    CHECK(runtime_get_executed_instructions(&runtime) >= 512);

    bytecode_free(&program);
    free_scope(&runtime.call_stack[0]);
    runtime_destroy(&runtime);
    puts("automatic GC safe-point tests passed");
    return 0;
}
